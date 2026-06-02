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
| Refatoração de `main.cpp`: constantes → parâmetros CLI `--chave valor` | Permitir configuração via launcher sem recompilar |
| Criação de `launcher.py` (v1) | Interface gráfica Tkinter para configurar e lançar o programa |
| Correção do launcher: remoção de `CREATE_NEW_CONSOLE` | Flag bloqueava a janela SDL2 de aparecer via WSLg |
| Redesign completo do `launcher.py` (v2) | UI anterior difícil de usar; spinboxes, dropdown, log integrado |

---

## 15. Interface gráfica — launcher.py

**Usuário:** Criar uma interface onde seja possível definir os parâmetros e apertar play para rodar, com tooltips explicando o que cada parâmetro afeta.

### Estratégia adotada

Em vez de criar uma tela de configuração dentro do SDL2 (trabalhoso, sem widgets nativos), foi criado um launcher Python/Tkinter que:
1. Coleta todos os parâmetros via formulário
2. Monta o comando WSL completo com `--chave valor`
3. Lança o processo via `subprocess.Popen`

Isso exigiu refatorar `main.cpp` para converter todas as `static const` hardcoded em parâmetros de linha de comando com `--chave valor`.

### Parâmetros expostos na interface

| Parâmetro CLI | Padrão | Descrição |
|---|---|---|
| `--threads N` | 4 | Threads paralelas |
| `--max-iter N` | 256 | Teto de iterações |
| `--block-size N` | 32 | Tamanho do bloco em pixels |
| `--width N` | 900 | Largura da janela |
| `--height N` | 900 | Altura da janela |
| `--zoom-x F` | -0.7436… | Coordenada X do ponto alvo |
| `--zoom-y F` | 0.1318… | Coordenada Y do ponto alvo |
| `--zoom-factor F` | 1.008 | Fator de zoom por frame |
| `--frame-delay N` | 0 | Delay entre frames (ms) |
| `--palette N` | 0 | Paleta de cores (0–4) |

### Parsing em main.cpp

```cpp
static const char* get_arg(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; i++)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return nullptr;
}

// Uso:
int num_threads = std::max(1, arg_int(argc, argv, "--threads", 4));
double zoom_x   = arg_dbl(argc, argv, "--zoom-x", -0.7436438885706799);
```

Todos os parâmetros têm valores padrão — o binário continua funcionando sem nenhum argumento.

### Paletas adicionadas ao main.cpp

A paleta foi adicionada ao `ViewState` e passada para `compute_pixel`:

```cpp
switch (palette) {
    case 1: // Fogo
        r = min(1.0, t*3.0);  g = min(1.0, max(0.0, t*3.0-1.0));  b = min(1.0, max(0.0, t*3.0-2.0));
        break;
    case 2: // Oceano
        r = min(1.0, max(0.0, t*3.0-2.0));  g = min(1.0, max(0.0, t*3.0-1.0));  b = min(1.0, t*3.0);
        break;
    case 3: // Gold & Purple
        r = 0.5+0.5*sin(6.28*(t*2+0.75));  g = 0.5+0.5*sin(6.28*(t*2+0.50));  b = 0.5+0.5*sin(6.28*(t*2));
        break;
    case 4: // Cinza
        r = t;  g = t;  b = t;
        break;
    default: // Padrão
        r = 0.5+0.5*sin(6.28*(t*3+0.00));  g = 0.5+0.5*sin(6.28*(t*3+0.33));  b = 0.5+0.5*sin(6.28*(t*3+0.67));
}
```

---

## 16. Bug: janela SDL2 não aparecia ao trocar parâmetros

**Usuário:** "Quando troquei alguns parâmetros, por exemplo tamanho da tela, não apareceu a janela renderizando."

**Causa identificada:**

O launcher v1 usava `subprocess.CREATE_NEW_CONSOLE` para abrir uma janela de terminal separada. Essa flag cria um processo em um ambiente isolado que **não herda as variáveis de display do WSLg**, impedindo o SDL2 de abrir a janela gráfica.

**Solução:**

Remover `CREATE_NEW_CONSOLE` e redirecionar `stdout`/`stderr` para dentro do próprio launcher:

```python
# Antes (v1) — bloqueava o WSLg
subprocess.Popen(
    ["wsl", "-d", "Ubuntu", "-e", "bash", "-c", bash_cmd],
    creationflags=subprocess.CREATE_NEW_CONSOLE,
)

# Depois (v2) — funciona corretamente
self._proc = subprocess.Popen(
    ["wsl", "-d", "Ubuntu", "-e", "bash", "-c", bash_cmd],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
```

A saída do programa agora aparece no log integrado do launcher em tempo real.

---

## 17. Redesign da interface (launcher v2)

**Usuário:** "Achei sua interface ruim de mexer, tem como deixar ela melhor?"

**Problemas identificados na v1:**
- Sliders difíceis de usar com precisão
- Botões `?` escondiam as descrições (precisava clicar para ver)
- Nenhum feedback de saída do programa
- Preset de zoom como botões soltos sem contexto visual
- `CREATE_NEW_CONSOLE` causava o bug de janela

**Melhorias implementadas na v2:**

| Componente | v1 | v2 |
|---|---|---|
| Controles numéricos | Sliders `ttk.Scale` | Spinboxes `ttk.Spinbox` com ▲▼ |
| Seleção de preset | 6 botões individuais | Dropdown `ttk.Combobox` com todos os presets |
| Descrições | Escondidas atrás do botão `?` | Sempre visíveis abaixo de cada campo |
| Saída do programa | Nenhuma | Log integrado com scroll |
| Status | Nenhum | Indicador ● Pronto / Compilando / Executando / Erro |
| Parar processo | Não havia | Botão "Parar" que termina o processo |
| Verificação do binário | Não havia | Detecta se `./mandelbrot` existe antes de executar |
| Compilação | Janela separada | Log integrado na mesma tela |

### Como executar

```bash
# Compilar (uma vez, ou após editar main.cpp)
wsl -d Ubuntu -e bash -c "cd '/mnt/c/.../mandelbrot' && make"

# Abrir o launcher
python launcher.py
```

O launcher verifica automaticamente se o binário existe. Se não existir, oferece compilar antes de executar.

---

## 18. Correção de parâmetros que não abriam a janela SDL2

**Usuário:** "Parece que alguns parâmetros da interface, assim que eu altero, mando compilar e executar, ele não está abrindo a interface com o Mandelbrot."

**Diagnóstico — dois problemas independentes:**

**Problema 1 — Variáveis de display não herdadas:**
O bash não-interativo lançado por `subprocess.Popen(["wsl", "-e", "bash", "-c", ...])` não herda automaticamente as variáveis `DISPLAY`, `WAYLAND_DISPLAY` e `XDG_RUNTIME_DIR` do WSLg. Sem elas o SDL2 não encontra o servidor gráfico e falha silenciosamente.

**Problema 2 — `tk.IntVar` em estado inválido:**
Quando o usuário edita um Spinbox digitando (não pelas setas), o `IntVar` pode conter temporariamente um valor não-inteiro. `_build_args()` chamava `.get()` sem proteção, lançando `TclError` que propagava sem mensagem visível.

**Soluções aplicadas:**

**Fix 1 — Constante `_WSL_ENV` injetada em todos os comandos bash:**
```python
_WSL_ENV = (
    "export DISPLAY=${DISPLAY:-:0}; "
    "export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}; "
    "export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}; "
)
bash_cmd = f"cd '{WSL_DIR}' && {_WSL_ENV}./mandelbrot ..."
```

**Fix 2 — Helpers `si`, `sf`, `ss` em `_build_args()`:**
```python
def si(var, default):
    try: return str(int(var.get()))
    except Exception: return str(default)
```
Qualquer campo inválido usa o valor padrão em vez de lançar exceção.

**Fix 3 — Try/except em `_do_run()`** com mensagem de erro visível ao usuário.

---

## 19. Bug: janela não abre na segunda execução

**Usuário:** "Assim que executei a primeira vez após suas alterações ele funcionou, porém depois que parou e tenta rodar de novo, a tela do Mandelbrot não aparece mais."

**Diagnóstico — dois problemas:**

**Problema 1 — Processo zumbi no WSL:**
`self._proc.terminate()` mata apenas o processo `wsl.exe` no lado Windows. O processo `mandelbrot` continuava rodando dentro do Linux, segurando a conexão com o WSLg. Na próxima tentativa de executar, a nova instância SDL2 não conseguia abrir janela.

**Problema 2 — Check de binário trava a UI:**
`subprocess.run(["wsl", ...check_binary...])` rodava na thread principal. Se o WSL estava reinicializando a distro Ubuntu (após o processo anterior ser encerrado abruptamente), esse check podia levar 5-10 segundos, congelando a interface.

**Soluções aplicadas:**

**Fix 1 — `pkill` antes de terminar o `wsl.exe`:**
```python
def _stop(self):
    if self._proc:
        subprocess.Popen(
            self._wsl_argv("pkill -TERM -x mandelbrot 2>/dev/null; true"),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self._proc.terminate()
```

**Fix 2 — Check de binário movido para thread de fundo:**
```python
def _run(self):
    if self._proc is not None:
        messagebox.showinfo("Em execução", "...")
        return
    # ...
    def _check():
        result = subprocess.run(self._wsl_argv("test -f .../mandelbrot && echo OK || echo MISSING"),
                                capture_output=True, text=True, timeout=20)
        found = result.returncode == 0 and "OK" in result.stdout
        if found:
            self.root.after(0, self._do_run)
        else:
            self.root.after(0, _ask)   # mostra diálogo na thread principal

    threading.Thread(target=_check, daemon=True).start()
```

---

## 20. Suporte a ambiente de execução dinâmico (WSL e Windows Nativo)

**Usuário:** "Quero poder definir onde vou rodar — no meu caso é WSL Ubuntu, mas meu colega roda direto no Windows com g++. Quero isso dinâmico, mas com os padrões já preenchidos."

**Solução:** Card **AMBIENTE DE EXECUÇÃO** (full-width, no topo da janela) com seletor de modo.

### Modo WSL

```
Distribuição WSL: [Ubuntu      ]   Caminho do projeto (WSL): [/mnt/c/.../mandelbrot  ↺]
```

- Compilação: `wsl -d <distro> -e bash -c "cd '<caminho>' && make"`
- Execução: `wsl -d <distro> -e bash -c "cd '<caminho>' && export DISPLAY=... && ./mandelbrot --key val ..."`

### Modo Windows Nativo

```
Caminho do projeto (Windows): [C:\Users\...\mandelbrot  ↺]
```

- Compilação: `g++ main.cpp -O2 -std=c++17 -Wall -Wextra -march=native -o main.exe -lmingw32 -lSDL2main -lSDL2` (via `subprocess.Popen`, `cwd=caminho_windows`)
- Execução: `main.exe --threads 4 --max-iter 256 ...` (mesmo formato de args `--key value`)

**Helpers criados:**
```python
def _distro(self)   -> str:  return self.var_wsl_distro.get().strip() or "Ubuntu"
def _wsldir(self)   -> str:  return self.var_wsl_path.get().strip()   or WSL_DIR
def _windir(self)   -> str:  return self.var_win_path.get().strip()   or SCRIPT_DIR
def _wsl_argv(self, bash_cmd: str) -> list:
    return ["wsl", "-d", self._distro(), "-e", "bash", "-c", bash_cmd]
def _on_mode_change(self):
    # pack_forget / pack dos frames WSL/Windows conforme o modo selecionado
```

Todos os subprocessos (`_compile`, `_do_run`, `_stop`, `_run._check`) passaram a usar esses helpers — sem mais hardcoded `"Ubuntu"` ou `WSL_DIR`.

---

## 21. Melhoria dos controles de Velocidade do Zoom e Delay

**Usuário:** "Melhore o jeito de poder informar o valor da velocidade de Zoom e do Delay entre frames."

**Problema:** os controles usavam `ttk.Scale` com um `tk.Label` fixo mostrando o valor atual. Para inserir um valor preciso (como `1.0035`) era necessário arrastar o slider com precisão milimétrica. Valores fora do intervalo do slider (`> 1.050` ou `> 500 ms`) eram impossíveis de inserir.

**Solução — `_card_slider` refatorado:**

Substituiu o `tk.Label` (read-only) por um `ttk.Entry` editável com sync bidirecional:

```python
# Slider → Entry: atualiza o campo a cada movimento
def _var_to_entry(*_):
    entry_var.set(fmt.format(var.get()))
var.trace_add("write", _var_to_entry)

# Entry → Var: aplica ao confirmar com Enter ou Tab (sem loop circular)
def _entry_to_var(_=None):
    val = max(lo, float(entry_var.get().strip().replace(",", ".")))
    if res < 1: val = round(round(val / res) * res, 6)
    else:       val = int(round(val))
    var.set(val)

entry.bind("<Return>",   _entry_to_var)
entry.bind("<FocusOut>", _entry_to_var)
```

**Comportamento resultante:**
- Arrastar o slider atualiza o campo em tempo real
- Digitar um valor no campo e pressionar Enter/Tab atualiza o slider (e clamp ao mínimo)
- Valores acima do range do slider (`> 1.050` para zoom, `> 500` para delay) são aceitos

---

## 22. Linha do tempo das alterações — continuação

| Alteração | Motivo |
|-----------|--------|
| Adição de `_WSL_ENV` nos comandos bash | SDL2 não encontrava o servidor gráfico do WSLg em shell não-interativo |
| `_build_args()` com helpers `si`/`sf`/`ss` | `IntVar.get()` lançava `TclError` para spinboxes em edição, sem mensagem ao usuário |
| `pkill mandelbrot` antes de `terminate()` no `_stop()` | Processo zumbi no Linux impedia nova janela SDL2 |
| Check de binário movido para thread de fundo | `subprocess.run` na thread principal congelava a UI durante reinicialização do WSL |
| Guard `_proc is not None` em `_run()` | Impede double-run acidental |
| Card AMBIENTE DE EXECUÇÃO com seletor WSL/Windows | Tornar o launcher usável em qualquer ambiente sem editar o código |
| Helpers `_distro`, `_wsldir`, `_windir`, `_wsl_argv` | Eliminar hardcoded `"Ubuntu"` e `WSL_DIR` dos subprocessos |
| `_card_slider`: `tk.Label` → `ttk.Entry` bidirecional | Permitir digitação direta e valores fora do range do slider |
| Tópico 2 adicionado ao README.md | Documentar a interface gráfica detalhadamente |
