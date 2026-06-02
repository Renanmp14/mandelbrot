# Fractal de Mandelbrot — Computação de Alto Desempenho

Renderizador interativo do fractal de Mandelbrot com **zoom contínuo e automático**, construído em C++ utilizando **Pthreads** para paralelismo e **SDL2** para renderização gráfica.

---

## Sumário

1. [Pré-requisitos](#pré-requisitos)
2. [Interface Gráfica — launcher.py](#interface-gráfica--launcherppy)
   - [Como executar o launcher](#como-executar-o-launcher)
   - [Ambiente de execução](#ambiente-de-execução)
   - [Seção Desempenho](#seção-desempenho)
   - [Seção Janela](#seção-janela)
   - [Seção Paleta de Cores](#seção-paleta-de-cores)
   - [Seção Ponto Alvo do Zoom](#seção-ponto-alvo-do-zoom)
   - [Seção Velocidade do Zoom](#seção-velocidade-do-zoom)
   - [Seção Delay Entre Frames](#seção-delay-entre-frames)
   - [Preview do Comando](#preview-do-comando)
   - [Botões e indicadores](#botões-e-indicadores)
3. [Como compilar](#como-compilar)
4. [Como executar](#como-executar)
5. [Parâmetros e seu efeito na renderização](#parâmetros-e-seu-efeito-na-renderização)
6. [Bibliotecas utilizadas](#bibliotecas-utilizadas)
7. [Arquivos do projeto](#arquivos-do-projeto)
8. [Explicação do código](#explicação-do-código)
   - [Estruturas de dados](#estruturas-de-dados)
   - [Algoritmo de Mandelbrot e coloração](#algoritmo-de-mandelbrot-e-coloração)
   - [Thread trabalhadora (worker)](#thread-trabalhadora-worker)
   - [Produção de tarefas e espera de frame](#produção-de-tarefas-e-espera-de-frame)
   - [Função `main`](#função-main)
9. [Arquitetura e sincronização](#arquitetura-e-sincronização)
10. [Personalizando a imagem (onde alterar no código)](#personalizando-a-imagem-onde-alterar-no-código)
11. [Testes de balanceamento e performance](#testes-de-balanceamento-e-performance)

---

## Pré-requisitos

### Sistema operacional

O projeto foi desenvolvido para **Linux** (nativo ou via WSL2 no Windows).

> **Windows:** todos os comandos abaixo devem ser executados **dentro do WSL2** (Ubuntu).  
> Para abrir um terminal WSL no Windows, execute `wsl -d Ubuntu` no PowerShell ou Prompt de Comando.

### Dependências

| Dependência | Versão mínima | Finalidade |
|-------------|---------------|------------|
| `g++` | 11+ (testado com 15.2.0) | Compilador C++ |
| `libSDL2-dev` | 2.0+ (testado com 2.32.10) | Renderização gráfica |
| `make` *(opcional)* | qualquer | Automatização da compilação |

### Instalação das dependências (Ubuntu/Debian)

```bash
# Atualiza a lista de pacotes
sudo apt-get update

# Instala o compilador, a SDL2 e o make
sudo apt-get install -y g++ libsdl2-dev make
```

### Requisito de display (WSL2 no Windows)

Para abrir a janela gráfica a partir do WSL2, o Windows precisa suportar **WSLg** (disponível no Windows 11 com WSL 2.0+) ou ter um servidor X instalado (ex.: VcXsrv). No Windows 11 atualizado, a janela abre automaticamente sem configuração extra.

---

## Interface Gráfica — launcher.py

O projeto inclui um **launcher** em Python/Tkinter que permite configurar todos os parâmetros visualmente, compilar e executar o programa sem abrir um terminal. É a forma recomendada de uso, especialmente para quem não está familiarizado com a linha de comando.

```
mandelbrot/
└── launcher.py   ← execute este arquivo com Python no Windows
```

---

### Como executar o launcher

**Pré-requisito:** Python 3.8+ instalado no Windows (o Tkinter já vem incluso).

```powershell
# No PowerShell, dentro da pasta do projeto:
python launcher.py
```

Ou dê dois cliques em `launcher.py` no Explorador de Arquivos se o Python estiver configurado como aplicativo padrão para `.py`.

---

### Ambiente de execução

O card **AMBIENTE DE EXECUÇÃO**, exibido logo abaixo do cabeçalho, define *como* o programa será compilado e executado. Ele tem dois modos, selecionáveis por botão de rádio:

#### Modo WSL (Linux) — padrão

Usado em máquinas Windows com WSL2 instalado (Ubuntu ou outra distro).

| Campo | Descrição | Padrão |
|-------|-----------|--------|
| **Distribuição WSL** | Nome exato da distro, conforme `wsl --list` | `Ubuntu` |
| **Caminho do projeto (WSL)** | Caminho da pasta em notação Linux (`/mnt/c/...`) | Detectado automaticamente |

O botão **↺** ao lado do caminho restaura o valor detectado automaticamente (pasta onde o `launcher.py` está).

- **Compilação:** executa `make` dentro do WSL na pasta configurada
- **Execução:** executa `./mandelbrot` com todos os parâmetros via `wsl -d <distro> -e bash -c "..."`
- **Parar:** envia `pkill -TERM mandelbrot` diretamente no Linux (evita que o processo fique ativo em background) e depois encerra o processo do lado Windows

#### Modo Windows Nativo

Usado em máquinas com g++ e SDL2 instalados diretamente no Windows (MSYS2/MinGW ou similar), sem WSL.

| Campo | Descrição | Padrão |
|-------|-----------|--------|
| **Caminho do projeto (Windows)** | Pasta onde estão `main.cpp` e onde `main.exe` será criado | Detectado automaticamente |

O botão **↺** ao lado do caminho restaura o valor detectado automaticamente.

- **Compilação:** executa `g++ main.cpp -O2 -std=c++17 -Wall -Wextra -march=native -o main.exe -lmingw32 -lSDL2main -lSDL2` diretamente no Windows, a partir da pasta configurada
- **Execução:** executa `main.exe` com todos os parâmetros, a partir da pasta configurada
- **Parar:** encerra o processo `main.exe` diretamente

> **Requisito do modo Windows:** `g++` e as bibliotecas SDL2 (headers + `.dll`) devem estar no `PATH` do sistema. Se o comando `g++ --version` funcionar no PowerShell, o launcher consegue compilar.

---

### Seção Desempenho

Localizada na **coluna esquerda**, controla os três parâmetros com maior impacto no FPS e na qualidade.

| Campo | Tipo | Intervalo | Padrão | Efeito |
|-------|------|-----------|--------|--------|
| **Threads** | Spinbox | 1 – 32 | 4 | Número de threads paralelas. Ganho linear até o nº de núcleos físicos da CPU. Use o valor de `nproc` para desempenho máximo. |
| **Máx. Iterações** | Spinbox | 32 – 4096 | 256 | Teto de iterações por pixel. Define o detalhe na fronteira do conjunto. Valores altos = mais detalhe, menos FPS. O programa cresce esse valor automaticamente em zoom profundo. |
| **Tamanho do Bloco** | Spinbox | 4 – 128 | 32 | Lado (em pixels) de cada bloco de trabalho. Blocos menores melhoram o balanceamento entre threads, especialmente em zoom profundo. |

Todos os campos são **Spinboxes**: use as setas ▲▼ para incremento/decremento ou clique no campo e digite o valor desejado diretamente.

---

### Seção Janela

Localizada na **coluna esquerda**, define as dimensões da janela SDL2 que será aberta.

| Campo | Tipo | Intervalo | Padrão |
|-------|------|-----------|--------|
| **Largura (px)** | Spinbox | 200 – 3840 | 900 |
| **Altura (px)** | Spinbox | 200 – 2160 | 900 |

A janela pode ter qualquer resolução dentro do intervalo. Janelas maiores consomem mais memória e aumentam o número de pixels a calcular por frame.

---

### Seção Paleta de Cores

Localizada na **coluna esquerda**, permite escolher a coloração do fractal por botão de rádio. A mudança é aplicada no próximo clique em **Executar**.

| Opção | Aparência |
|-------|-----------|
| **Padrão** | Canais R, G, B defasados 120° — resultado: azul/verde/roxo ciclando suavemente |
| **Fogo** | Preto → vermelho → laranja → amarelo |
| **Oceano** | Preto → azul escuro → ciano |
| **Gold & Purple** | Ciclo arco-íris com dominância dourada e roxa |
| **Cinza** | Escala de cinza pura — útil para análise de estrutura |

---

### Seção Ponto Alvo do Zoom

Localizada na **coluna direita**, define o ponto do plano complexo para onde a câmera avança.

> **Importante:** o ponto deve estar exatamente na **fronteira** do conjunto de Mandelbrot. Pontos no interior de um "lago" (bulbo ou cardioide) fazem a tela ficar completamente preta em zoom profundo — é matematicamente correto, mas visualmente parece travado.

#### Preset

Um menu dropdown com 6 pontos pré-validados:

| Preset | X | Y | Característica visual |
|--------|---|---|-----------------------|
| **Seahorse Valley** *(padrão)* | `-0.7436…` | `0.1318…` | Espirais em ferradura, muito rico em detalhe |
| **Elephant Valley** | `0.3245…` | `0.0485…` | Protuberâncias em forma de elefante |
| **Triple Spiral** | `-0.1011` | `0.9563` | Três espirais simétricas no topo |
| **Mini Mandelbrot** | `-1.7497…` | `0.0` | Cópia em miniatura do conjunto inteiro |
| **Double Spiral** | `-0.7771` | `0.1166` | Espiral dupla densa |
| **Lightning** | `-0.5990` | `0.6500` | Estruturas em forma de relâmpago |

Ao selecionar um preset, os campos **X** e **Y** abaixo são preenchidos automaticamente.

#### X (parte real) e Y (parte imaginária)

Campos de texto livres. Permitem inserir qualquer coordenada decimal. Use `.` como separador decimal.

---

### Seção Velocidade do Zoom

Localizada na **coluna direita**. Controla quanto a câmera avança por frame — quanto maior o valor, mais rápido o zoom entra.

O controle combina **slider** (para ajuste rápido por arrasto) com **campo de texto editável** (para valores precisos):

- **Slider:** intervalo de 1.001 a 1.050, passo 0.001
- **Campo de texto:** aceita qualquer valor maior que 1.001 — basta digitar e pressionar **Enter** ou **Tab**. Valores acima de 1.050 (como `1.10` ou `1.20`) são aceitos e aplicados sem restrição.

| Valor | Sensação |
|-------|----------|
| `1.003` | Muito lento, cinematográfico |
| `1.008` | Lento e suave ← **padrão** |
| `1.015` | Moderado |
| `1.030` | Rápido |
| `1.050` | Muito rápido |
| `> 1.050` | Extremamente rápido (digitar no campo) |

> **Lógica:** a escala é dividida pelo fator a cada frame (`scale /= zoom_factor`). Um fator maior que 1 faz `scale` diminuir, o que aumenta o zoom. Um fator de exatamente `1.0` pararia o zoom.

---

### Seção Delay Entre Frames

Localizada na **coluna direita**. Insere uma pausa mínima entre frames, desacoplando a velocidade de renderização da velocidade do zoom.

O controle combina **slider** (para ajuste rápido) com **campo de texto editável** (para valores precisos):

- **Slider:** intervalo de 0 a 500 ms, passo 1
- **Campo de texto:** aceita qualquer valor ≥ 0 — basta digitar e pressionar **Enter** ou **Tab**. Valores acima de 500 ms (como `1000` ou `2000`) são aceitos e aplicados.

| Valor | Efeito |
|-------|--------|
| `0` | Sem limite — máxima velocidade de renderização ← **padrão** |
| `16` | ≈ 60 FPS máximos |
| `33` | ≈ 30 FPS máximos |
| `100` | ≈ 10 FPS máximos |
| `1000` | 1 frame por segundo (digitar no campo) |

> **Diferença entre Delay e Zoom Factor:** o *Zoom Factor* controla **quanto** a câmera avança por frame; o *Delay* controla **de quanto em quanto tempo** um novo frame é renderizado. Para uma animação lenta e suave, use Zoom Factor próximo de `1.003` com Delay `0`. Para economizar CPU mantendo o zoom visual igual, aumente o Delay sem alterar o Zoom Factor.

---

### Preview do Comando

Exibido logo abaixo dos cards de parâmetros, o campo **Comando** mostra em tempo real o comando exato que será executado ao clicar em **Executar**. Qualquer alteração nos parâmetros ou no ambiente atualiza o preview imediatamente.

**Exemplo — modo WSL:**
```
wsl -d Ubuntu -e bash -c "cd '/mnt/c/Users/.../mandelbrot' && export DISPLAY=... && ./mandelbrot --threads 8 --max-iter 256 ..."
```

**Exemplo — modo Windows:**
```
.\main.exe --threads 8 --max-iter 256 --block-size 16 --width 900 --height 900 ...
```

---

### Botões e indicadores

#### Rodapé da interface

| Elemento | Função |
|----------|--------|
| **Compilar (make)** | Compila o projeto. No modo WSL executa `make`; no modo Windows executa `g++` diretamente. A saída da compilação aparece no log. |
| **Parar** | Encerra o processo em execução. Habilitado apenas enquanto o programa está rodando. No modo WSL envia `pkill` antes de encerrar o processo Windows, evitando processos zumbi. |
| **● indicador** | Mostra o estado atual: verde = Pronto/Encerrado, amarelo = Compilando/Executando, vermelho = Erro/Parado. |
| **▶ EXECUTAR** | Verifica se o binário existe (em background, sem travar a interface), e inicia o programa com os parâmetros configurados. Se o binário não existir, oferece compilar antes de executar. |

#### Log de saída

Painel de texto na parte inferior da janela que exibe em tempo real:
- O comando executado (em azul)
- Todo o `stdout` e `stderr` do programa (banner de inicialização, erros SDL2, etc.)
- Mensagem de encerramento com o código de saída

---

## Como compilar

### Usando `build.sh` (sem dependência de `make`)

```bash
# A partir do Windows (PowerShell):
wsl -d Ubuntu -e bash "/mnt/c/Users/<SEU_USUARIO>/Documents/_Faculdade/mandelbrot/build.sh"

# Ou já dentro do WSL:
bash build.sh
```

### Usando `make`

```bash
# Dentro da pasta do projeto no WSL:
make

# Para limpar o binário:
make clean
```

### Compilação manual com `g++`

```bash
g++ -O2 -std=c++17 -Wall -Wextra -march=native \
    -o mandelbrot main.cpp \
    -lSDL2 -lpthread -lm
```

> **Flag `-march=native`:** habilita otimizações específicas do processador (SSE, AVX), o que acelera significativamente o loop interno de cálculo — relevante para um trabalho de Alto Desempenho.

---

## Como executar

```bash
./mandelbrot <num_threads> <max_iter> <block_size>
```

### A partir do Windows (PowerShell)

```powershell
wsl -d Ubuntu -e bash -c "cd '/mnt/c/Users/<SEU_USUARIO>/Documents/_Faculdade/mandelbrot' && ./mandelbrot 4 256 32"
```

### Exemplos de uso

```bash
# Configuração padrão: 4 threads, 256 iterações, blocos 32×32
./mandelbrot 4 256 32

# Alta qualidade: 8 threads, 512 iterações, blocos 16×16
./mandelbrot 8 512 16

# Teste de desempenho: 1 thread (sem paralelismo)
./mandelbrot 1 256 32

# Alta complexidade: 16 threads, 1024 iterações
./mandelbrot 16 1024 32
```

### Controles durante a execução

| Ação | Efeito |
|------|--------|
| Fechar a janela | Encerra o programa |
| Tecla `ESC` | Encerra o programa |

O título da janela exibe em tempo real: **FPS atual**, **fator de zoom** e **número de threads**.

---

## Parâmetros e seu efeito na renderização

### `num_threads` — Número de threads trabalhadoras

Controla quantas threads paralelas calculam os blocos da imagem simultaneamente.

| Valor | Efeito |
|-------|--------|
| `1` | Sem paralelismo — modo de referência para medir ganho |
| `2–4` | Ganho expressivo em CPUs dual/quad-core |
| `8–16` | Ideal para CPUs modernas com muitos núcleos |
| `> núcleos físicos` | Pode haver queda de desempenho por contenção de contexto |

> **Experimento didático:** execute com `1`, `2`, `4` e `8` threads e compare o FPS exibido no título da janela para observar o ganho de escalabilidade.

---

### `max_iter` — Máximo de iterações

Define o limite de iterações do algoritmo para determinar se um ponto pertence ao conjunto de Mandelbrot.

| Valor | Efeito visual | Custo computacional |
|-------|---------------|---------------------|
| `64` | Fronteiras grossas, pouco detalhe | Muito rápido |
| `128` | Detalhe moderado | Rápido |
| `256` | Boa qualidade (padrão) | Médio |
| `512` | Alta qualidade, detalhes finos nas fronteiras | Lento |
| `1024+` | Detalhe extremo em zooms profundos | Muito lento |

> **Como afeta a imagem:** pontos próximos à fronteira do conjunto precisam de mais iterações para serem classificados corretamente. Com poucos iterações, essas regiões ficam com cores erradas ou parecem pertencer ao conjunto (preto). Com mais iterações, as fronteiras ficam mais nítidas e detalhadas — essencial em zooms profundos.

---

### `block_size` — Tamanho dos blocos de tarefa

Define o tamanho (em pixels) de cada bloco quadrado em que a tela é dividida. Cada bloco é uma **tarefa** na fila do pool de threads.

| Valor | Efeito no desempenho | Observação |
|-------|---------------------|------------|
| `4–8` | Muitas tarefas pequenas, boa distribuição de carga | Overhead maior de sincronização |
| `16–32` | Equilíbrio ideal (padrão recomendado) | Melhor para a maioria dos casos |
| `64–128` | Poucas tarefas grandes | Threads podem ficar ociosas |
| `900` | Uma única tarefa = sem paralelismo real | Apenas para teste |

> **Granularidade da tarefa:** blocos muito pequenos aumentam o overhead de mutex/fila; blocos muito grandes prejudicam o balanceamento de carga, pois regiões com muitas iterações (fronteira do conjunto) demoram muito mais que regiões internas (preto) ou externas.

---

## Bibliotecas utilizadas

### `SDL2` — Simple DirectMedia Layer 2

**Header:** `<SDL2/SDL.h>`  
**Link:** `-lSDL2`  
**Uso no projeto:** criação da janela, gerenciamento de eventos (fechar/ESC), textura de streaming para exibir o buffer de pixels calculado pelas threads.

Componentes utilizados:
- `SDL_Init` / `SDL_Quit` — inicialização e encerramento
- `SDL_CreateWindow` — cria a janela 900×900
- `SDL_CreateRenderer` — renderizador acelerado por hardware
- `SDL_CreateTexture` (ARGB8888, STREAMING) — textura atualizada a cada frame
- `SDL_UpdateTexture` — copia o buffer de pixels para a GPU
- `SDL_RenderCopy` + `SDL_RenderPresent` — exibe na tela
- `SDL_PollEvent` — captura eventos de teclado e janela

---

### `Pthreads` — POSIX Threads

**Header:** `<pthread.h>`  
**Link:** `-lpthread`  
**Uso no projeto:** criação e gerenciamento do pool de threads trabalhadoras, e todos os primitivos de sincronização.

Primitivos utilizados:
| Primitivo | Uso |
|-----------|-----|
| `pthread_t` | Identificador de thread |
| `pthread_create` | Criação das worker threads |
| `pthread_join` | Espera pelo encerramento de cada thread |
| `pthread_mutex_t` | Exclusão mútua na fila de tarefas e na sinalização de frame |
| `pthread_mutex_lock/unlock` | Acesso seguro às estruturas compartilhadas |
| `pthread_cond_t` | Variável de condição para bloqueio eficiente |
| `pthread_cond_wait` | Workers aguardam tarefas; main aguarda frame |
| `pthread_cond_signal` | Acorda a main quando o frame termina |
| `pthread_cond_broadcast` | Acorda todas as workers quando há novas tarefas |

---

### `std::atomic<int>` — Atômico da STL C++

**Header:** `<atomic>`  
**Uso:** contador `tasks_remaining` — decrementado por cada worker ao terminar sua tarefa, sem necessidade de mutex.

---

### Biblioteca padrão C/C++

| Header | Uso |
|--------|-----|
| `<cmath>` | `std::log`, `std::sin` — cálculo matemático do fractal e paleta de cores |
| `<queue>` | `std::queue<Task>` — fila FIFO de tarefas |
| `<vector>` | `std::vector` — lista de threads e tarefas por frame |
| `<algorithm>` | `std::min` — limita bordas dos blocos à tela |
| `<cstdio>` | `printf`, `fprintf`, `snprintf` — saída de informações |
| `<cstdlib>` | `std::atoi` — leitura dos parâmetros da linha de comando |
| `<cstdint>` | `uint32_t`, `uint64_t` — tipos inteiros de tamanho fixo |

---

## Arquivos do projeto

```
mandelbrot/
├── main.cpp      Código-fonte principal (~270 linhas)
├── Makefile      Script de compilação com make
├── build.sh      Script alternativo de compilação (sem make)
└── README.md     Este arquivo
```

### `main.cpp`

Arquivo único contendo toda a implementação:
- Definição das estruturas de dados compartilhadas
- Algoritmo de Mandelbrot com smooth coloring
- Lógica da thread trabalhadora
- Funções de despacho e espera de frame
- Função `main` com inicialização, loop de renderização e encerramento

### `Makefile`

Automatiza a compilação com `make` (ou `make run` para compilar e executar, `make clean` para remover o binário).

### `build.sh`

Script Bash equivalente ao `make all`, útil quando `make` não está instalado.

---

## Explicação do código

### Estruturas de dados

#### `Task`

```cpp
struct Task {
    int x0, y0;   // canto superior esquerdo (inclusivo)
    int x1, y1;   // canto inferior direito (exclusivo)
};
```

Representa um **bloco retangular de pixels** a ser calculado. A tela de 900×900 é dividida em múltiplos blocos — cada `Task` é uma unidade de trabalho na fila do pool de threads.

---

#### `ViewState`

```cpp
struct ViewState {
    double cx, cy;   // centro da visão no plano complexo
    double scale;    // unidades complexas por pixel
    int    max_iter; // máximo de iterações
};
```

Captura o **estado de visão do frame atual**: onde o centro está no plano complexo e o nível de zoom (`scale`). Quanto menor o `scale`, maior o zoom. As worker threads fazem uma cópia local desse estado no início de cada tarefa, garantindo consistência mesmo que a main thread avance o zoom para o próximo frame.

---

#### `SharedCtx`

```cpp
struct SharedCtx {
    std::queue<Task>  task_queue;     // fila de tarefas
    pthread_mutex_t   task_mutex;     // protege task_queue
    pthread_cond_t    task_cond;      // acorda workers quando há tarefas

    uint32_t*         pixels;         // buffer de resultado (sem mutex)

    ViewState         view;           // estado de visão atual

    std::atomic<int>  tasks_remaining; // tarefas ainda pendentes no frame
    pthread_mutex_t   done_mutex;
    pthread_cond_t    done_cond;       // acorda a main quando frame termina

    bool              shutdown;        // sinaliza encerramento
};
```

O **contexto compartilhado** entre todas as threads. É o núcleo da arquitetura:

- **`task_queue` + `task_mutex` + `task_cond`:** implementam o padrão produtor/consumidor clássico para distribuir tarefas às workers.
- **`pixels`:** buffer linear de `WIN_W × WIN_H` pixels em formato ARGB. Não precisa de mutex porque cada `Task` cobre regiões de memória disjuntas — não há dois threads escrevendo no mesmo endereço.
- **`tasks_remaining`:** contador atômico que a main thread inicializa com N (número de tarefas) e cada worker decrementa ao concluir. Quando chega a zero, o frame está completo.
- **`done_mutex` + `done_cond`:** usados pela última worker para acordar a main thread bloqueada em `wait_frame_done()`.

---

### Algoritmo de Mandelbrot e coloração

```cpp
static uint32_t compute_pixel(double px, double py, int max_iter)
```

Recebe um ponto `(px, py)` no plano complexo e retorna a cor ARGB `0xAARRGGBB`.

**Iteração principal:**

```
z_{n+1} = z_n² + c,    z_0 = 0,    c = px + i·py
```

O loop continua enquanto `|z|² ≤ 4` e `iter < max_iter`. Quando `|z|² > 4`, o ponto diverge (exterior do conjunto). Quando `iter == max_iter`, assume-se que o ponto pertence ao conjunto (retorna preto).

**Smooth coloring (coloração suavizada):**

O problema da coloração por iteração inteira é que pontos com a mesma contagem de iterações formam "faixas" bruscas de cor. A técnica de smooth coloring usa o módulo final de `z` para interpolar:

```
log_zn  = log(|z|)
nu      = log₂( log₂(|z|) )
smooth  = (iter + 1) - nu        ← valor contínuo
```

Isso produz um valor fracionário que varia suavemente entre iterações adjacentes.

**Paleta de cores:**

Três funções seno defasadas em 120° geram as componentes RGB de forma cíclica e contínua:

```cpp
double t = smooth / max_iter;
r = 0.5 + 0.5 * sin(2π × (t×3 + 0.00))   // vermelho
g = 0.5 + 0.5 * sin(2π × (t×3 + 0.33))   // verde
b = 0.5 + 0.5 * sin(2π × (t×3 + 0.67))   // azul
```

O fator `3.0` controla a "densidade" das faixas coloridas — valores maiores criam mais bandas por zoom.

---

### Thread trabalhadora (worker)

```cpp
static void* worker_func(void* arg)
```

Cada worker thread executa este loop infinito:

```
1. Adquire task_mutex
2. Enquanto task_queue vazia e !shutdown:
       pthread_cond_wait(task_cond, task_mutex)   ← bloqueia sem consumir CPU
3. Se fila ainda vazia (shutdown): desbloqueia e encerra
4. Retira Task da frente da fila
5. Libera task_mutex
6. Copia ViewState localmente (v = ctx->view)
7. Para cada pixel (px, py) do bloco:
       cx = v.cx + (px - WIN_W/2) × v.scale
       cy = v.cy + (py - WIN_H/2) × v.scale
       pixels[py × WIN_W + px] = compute_pixel(cx, cy, v.max_iter)
8. Decrementa tasks_remaining atomicamente
   Se chegou a 0: sinaliza done_cond
9. Volta ao passo 1
```

**Ponto crítico — cópia do ViewState:** a worker faz `ViewState v = ctx->view` logo após retirar a tarefa. Isso é seguro porque a main thread só modifica `ctx->view` após `wait_frame_done()` retornar, ou seja, após todas as workers terem terminado o frame anterior. Não há corrida de dados.

**Ponto crítico — sem mutex no buffer de pixels:** cada `Task` define uma região retangular única da tela. Duas workers nunca calculam a mesma região, portanto nunca escrevem no mesmo endereço do array `pixels`. A sincronização por exclusão mútua seria desnecessária e custosa.

---

### Produção de tarefas e espera de frame

#### `dispatch_frame`

```cpp
static void dispatch_frame(SharedCtx* ctx, int block_size)
```

Divide a tela em blocos `block_size × block_size` e os enfileira:

1. Gera todos os objetos `Task` com suas coordenadas
2. **Define `tasks_remaining = N` ANTES de enfileirar** — garante que o contador já está correto mesmo que uma worker processe muito rapidamente
3. Adquire `task_mutex`, empurra todas as tarefas, chama `pthread_cond_broadcast` para acordar **todas** as workers em espera simultaneamente
4. Libera `task_mutex`

#### `wait_frame_done`

```cpp
static void wait_frame_done(SharedCtx* ctx)
```

Bloqueia a main thread até que `tasks_remaining == 0`:

```cpp
pthread_mutex_lock(&ctx->done_mutex);
while (ctx->tasks_remaining.load() > 0)
    pthread_cond_wait(&ctx->done_cond, &ctx->done_mutex);
pthread_mutex_unlock(&ctx->done_mutex);
```

**Por que não há "lost wakeup" aqui?** O padrão clássico de condition variable exige verificar a condição **enquanto se mantém o mutex**. Se a última worker decrementar `tasks_remaining` para zero e sinalizar **antes** de `wait_frame_done` adquirir `done_mutex`, o `while` verá `tasks_remaining == 0` imediatamente e não chamará `cond_wait` — correto. Se a sinalização vier **depois**, o `cond_wait` irá recebê-la e acordar — também correto.

---

### Função `main`

A `main` é responsável por:

**1. Inicialização**
- Lê e valida os 3 parâmetros da linha de comando
- Aloca o buffer de pixels (`new uint32_t[WIN_W * WIN_H]`)
- Inicializa os 4 primitivos de sincronização (`task_mutex`, `task_cond`, `done_mutex`, `done_cond`)
- Define o estado de visão inicial: centro em `(-0.7269, 0.1889)` (Seahorse Valley), escala aberta

**2. Criação do pool de threads**

```cpp
for (int i = 0; i < num_threads; i++)
    pthread_create(&workers[i], nullptr, worker_func, &ctx);
```

Todas as threads são criadas de uma vez e ficam aguardando tarefas na `task_queue`.

**3. Inicialização do SDL2**

Cria janela (900×900), renderizador acelerado por hardware e textura de streaming no formato `ARGB8888`.

**4. Loop principal**

```
while (running):
    SDL_PollEvent()          ← verifica ESC / fechar janela
    dispatch_frame()         ← enfileira N blocos
    wait_frame_done()        ← bloqueia até todas as workers terminarem
    SDL_UpdateTexture()      ← copia buffer de pixels para a GPU
    SDL_RenderCopy/Present() ← exibe na tela
    ctx.view.scale /= 1.018  ← avança zoom (−1.8% de escala por frame)
    atualiza título com FPS e zoom
    if scale < 1e-13:        ← limite de precisão do double → reinicia
        scale = SCALE_INITIAL
```

**5. Encerramento limpo**

Ativa a flag `shutdown`, chama `pthread_cond_broadcast` para acordar todas as workers e chama `pthread_join` em cada uma, garantindo que todas terminem antes de destruir os recursos.

---

## Arquitetura e sincronização

```
┌─────────────────────────────────────────────────────────┐
│                    Main Thread (Produtor)                │
│                                                         │
│  ctx.view ──── estado de zoom (escala, centro, max_iter)│
│  ctx.pixels ── buffer 900×900 de pixels ARGB            │
│                                                         │
│  dispatch_frame():                                      │
│    tasks_remaining = N                                  │
│    para cada bloco → task_queue.push(Task)              │
│    pthread_cond_broadcast(task_cond) ──────────────┐    │
│                                                    │    │
│  wait_frame_done():                                │    │
│    pthread_cond_wait(done_cond) ◄──────────────┐   │    │
│                                                │   │    │
│  SDL_UpdateTexture() → RenderPresent()         │   │    │
│  ctx.view.scale /= ZOOM_FACTOR                 │   │    │
└─────────────────────────────────────────────────┼───┼───┘
                                                  │   │
            ┌─────────────────────────────────────┼───┘
            │   Worker Threads × N (Consumidoras) │
            │                                     │
            │  loop:                              │
            │    pthread_cond_wait(task_cond) ◄───┘
            │    pop Task{x0,y0,x1,y1}
            │    v = ctx.view  (cópia local)
            │    para cada pixel do bloco:
            │      cx = v.cx + (px - W/2) × v.scale
            │      cy = v.cy + (py - H/2) × v.scale
            │      pixels[y×W+x] = compute_pixel(cx,cy)
            │    if (--tasks_remaining == 0):
            │      pthread_cond_signal(done_cond) ─────────►
            └─────────────────────────────────────────────────
```

### Garantias de correção

| Situação | Mecanismo |
|----------|-----------|
| Acesso concorrente à `task_queue` | `task_mutex` |
| Workers ficam ociosas sem consumir CPU | `pthread_cond_wait(task_cond)` |
| Main aguarda frame sem busy-wait | `pthread_cond_wait(done_cond)` |
| Sem corrida no buffer de pixels | Regiões disjuntas por design |
| Sem corrida no `ViewState` | Main só altera após `wait_frame_done()` |
| Decremento seguro de `tasks_remaining` | `std::atomic<int>` |
| Encerramento sem deadlock | `shutdown` + `broadcast` acordam todas as workers |

---

## Personalizando a imagem (onde alterar no código)

Todas as mudanças visuais estão concentradas na seção **`CONFIGURAÇÕES GLOBAIS`** no topo de `main.cpp` (linhas 18–35) e na função **`compute_pixel`** (linha ~65). Nenhuma outra parte do código precisa ser tocada.

---

### 1. Ponto de zoom — para onde a câmera entra

**Arquivo:** `main.cpp`  
**Linhas:** `ZOOM_TARGET_X` e `ZOOM_TARGET_Y`

> **Importante:** o ponto de zoom deve estar exatamente na **fronteira** do conjunto de Mandelbrot. Pontos levemente no interior de um "lago" (bulbo ou cardioid) fazem a tela ficar inteiramente preta em zoom profundo — é matematicamente correto, mas visualmente parece travado. Use coordenadas documentadas abaixo.

```cpp
// Seahorse Valley — ponta de espiral na fronteira (padrão atual)
static const double ZOOM_TARGET_X = -0.7436438885706799;
static const double ZOOM_TARGET_Y =  0.1318259042053185;
```

Troque pelos valores abaixo para explorar outras regiões famosas:

| Região | `ZOOM_TARGET_X` | `ZOOM_TARGET_Y` | Característica |
|--------|----------------|----------------|----------------|
| **Seahorse Valley** *(atual)* | `-0.7269450287` | `0.1889241660` | Espirais em forma de ferradura |
| **Elephant Valley** | `0.3245046418` | `0.0485510112` | Estruturas em forma de elefante |
| **Triple Spiral** | `-0.1011` | `0.9563` | Espirais triplas no topo do conjunto |
| **Mini Mandelbrot** | `-1.7497052` | `0.0` | Cópia reduzida do conjunto inteiro |
| **Double Spiral** | `-0.7771` | `0.1166` | Espiral dupla densa |
| **Quad Spiral** | `-0.1592` | `1.0317` | Quatro espirais simétricas |

> **Como descobrir novos pontos:** o eixo X corresponde à parte real e o eixo Y à parte imaginária do número complexo `c`. Qualquer ponto próximo à fronteira do conjunto gera zoom interessante.

---

### 2. Velocidade do zoom

**Arquivo:** `main.cpp`  
**Linha:** `ZOOM_FACTOR`

```cpp
// Fator multiplicativo de zoom por frame
static const double ZOOM_FACTOR = 1.018;  // padrão: +1.8% por frame
```

| Valor | Sensação | Uso recomendado |
|-------|----------|-----------------|
| `1.005` | Muito lento, cinematográfico | Apresentações, vídeos |
| `1.010` | Lento e suave | Demonstrações didáticas |
| `1.018` | **Médio (padrão)** | Uso geral |
| `1.030` | Rápido | Testes de desempenho |
| `1.050` | Muito rápido | Benchmark / stress test |

---

### 3. Visão inicial (nível de zoom ao iniciar)

**Arquivo:** `main.cpp`  
**Linha:** `SCALE_INITIAL`

```cpp
// Escala inicial: 3.5 unidades complexas divididas pela largura da janela
static const double SCALE_INITIAL = 3.5 / WIN_W;
```

O valor `3.5 / WIN_W` enquadra o conjunto de Mandelbrot inteiro na tela ao iniciar. Para começar já mais aproximado:

```cpp
// Começa 10× mais próximo do alvo
static const double SCALE_INITIAL = 0.35 / WIN_W;

// Começa 100× mais próximo (já entra em detalhe)
static const double SCALE_INITIAL = 0.035 / WIN_W;
```

---

### 4. Paleta de cores

**Arquivo:** `main.cpp`  
**Função:** `compute_pixel` — bloco de cálculo de `r`, `g`, `b` (por volta da linha 90)

```cpp
// Código atual — paleta vibrante tricolor
double t = smooth / (double)max_iter;
double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.00));
double g = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.33));
double b = 0.5 + 0.5 * std::sin(6.28318 * (t * 3.0 + 0.67));
```

Substitua as três linhas de `r`, `g`, `b` por uma das opções abaixo:

#### Paleta Fogo (preto → vermelho → laranja → amarelo → branco)
```cpp
double t = smooth / (double)max_iter;
double r = std::min(1.0, t * 3.0);
double g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
double b = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
```

#### Paleta Oceano (preto → azul → ciano → branco)
```cpp
double t = smooth / (double)max_iter;
double r = std::min(1.0, std::max(0.0, t * 3.0 - 2.0));
double g = std::min(1.0, std::max(0.0, t * 3.0 - 1.0));
double b = std::min(1.0, t * 3.0);
```

#### Paleta Gold & Purple (roxo → dourado → branco)
```cpp
double t = smooth / (double)max_iter;
double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.75));
double g = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.50));
double b = 0.5 + 0.5 * std::sin(6.28318 * (t * 2.0 + 0.00));
```

#### Paleta Escala de Cinza
```cpp
double t = smooth / (double)max_iter;
double r = t;
double g = t;
double b = t;
```

#### Ajustando a densidade de faixas (qualquer paleta senoidal)

O fator `3.0` nas funções seno controla quantas bandas de cor aparecem por nível de zoom. Aumente para mais bandas, diminua para gradientes mais suaves:

```cpp
// Poucas faixas (gradiente suave)
double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 1.0 + 0.00));

// Muitas faixas (detalhe psicodélico)
double r = 0.5 + 0.5 * std::sin(6.28318 * (t * 8.0 + 0.00));
```

---

### 5. Tamanho da janela

**Arquivo:** `main.cpp`  
**Linhas:** `WIN_W` e `WIN_H`

```cpp
static const int WIN_W = 900;
static const int WIN_H = 900;
```

Troque por qualquer resolução desejada (ex.: `1280` × `720`, `1920` × `1080`). A imagem e o mapeamento de coordenadas se ajustam automaticamente.

---

### Resumo rápido — onde fica cada coisa

```
main.cpp
│
├── linha ~18  →  WIN_W, WIN_H          (tamanho da janela)
├── linha ~22  →  ZOOM_TARGET_X/Y       (ponto de destino do zoom)
├── linha ~26  →  SCALE_INITIAL         (visão inicial / nível de entrada)
├── linha ~29  →  ZOOM_FACTOR           (velocidade do zoom)
│
└── função compute_pixel (~linha 65)
    └── bloco r, g, b                   (paleta de cores)
```

---

## Testes de balanceamento e performance

O título da janela exibe **FPS** e **Zoom atual** em tempo real — use esses valores para comparar cada configuração abaixo.

> Todos os comandos assumem que você está dentro da pasta do projeto no WSL.  
> A partir do Windows (PowerShell), prefixe com:  
> `wsl -d Ubuntu -e bash -c "cd '/mnt/c/Users/<SEU_USUARIO>/Documents/_Faculdade/mandelbrot' && <comando>"`

---

### O que cada parâmetro faz

```
./mandelbrot <num_threads> <max_iter> <block_size>
```

#### `num_threads` — Quantidade de threads trabalhadoras

Cada thread é um worker independente que pega blocos da fila de tarefas e calcula os pixels. Mais threads = mais blocos processados em paralelo.

- **Impacto direto no FPS** — é o parâmetro com maior efeito na velocidade geral
- **Limite prático** — o ganho de FPS cresce até o número de núcleos físicos do CPU; acima disso, as threads passam a disputar os mesmos núcleos e o ganho para ou cai
- **Não afeta a qualidade visual** — apenas a velocidade de cálculo

```
1 thread  → processa 1 bloco por vez   (sem paralelismo)
4 threads → processa 4 blocos em paralelo
8 threads → processa 8 blocos em paralelo
```

---

#### `max_iter` — Teto de iterações do algoritmo

Define o número máximo de vezes que a fórmula `z = z² + c` é aplicada antes de declarar que um ponto pertence ao conjunto (interior preto).

- **Valor baixo (64–128):** renderização rápida, fronteira do conjunto aparece "grossa" e sem detalhe — pixels que precisariam de mais iterações para escapar são classificados como interiores
- **Valor alto (512–1024):** fronteira muito detalhada, mas cada pixel de fronteira custa mais — FPS cai, especialmente em zoom profundo
- **Em zoom profundo**, o programa aumenta esse valor automaticamente (de forma suave) para manter o detalhe visível

> O programa usa esse valor como **base** e o aumenta dinamicamente conforme o zoom avança. Passar `256` não significa que sempre serão 256 iterações — pode chegar a `256 × 4 = 1024` em zoom extremo.

```
max_iter baixo → fronteira grossa, rápido
max_iter alto  → fronteira fina e detalhada, mais lento
```

---

#### `block_size` — Tamanho dos blocos de tarefa (em pixels)

A tela é dividida em blocos quadrados de `block_size × block_size` pixels. Cada bloco é uma tarefa na fila do pool de threads.

- **Blocos grandes (64–128):** poucas tarefas na fila — se um bloco cair exatamente sobre a fronteira do conjunto (onde os cálculos são pesados), a thread responsável vai demorar muito enquanto as outras ficam ociosas esperando novas tarefas
- **Blocos pequenos (8–16):** muitas tarefas na fila — as threads terminam rápido e sempre encontram mais trabalho disponível, distribuindo melhor a carga
- **Blocos muito pequenos (< 8):** o overhead de pegar/devolver tarefas do mutex começa a superar o ganho de balanceamento

> Este é o parâmetro mais importante para **zoom profundo**, quando a fronteira domina a tela e os blocos ficam com custo muito desigual.

```
block_size=128 → 49 tarefas   → desequilíbrio alto em zoom profundo
block_size=32  → 793 tarefas  → balanceamento razoável
block_size=16  → 3164 tarefas → balanceamento bom
block_size=8   → 12656 tarefas → overhead de mutex começa a aparecer
```

---

### Passo 0 — Verificar núcleos disponíveis

```bash
nproc
```

Use o valor retornado como referência para o parâmetro `num_threads` nos testes abaixo.

---

### Teste 1 — Impacto do número de threads

Mantém `max_iter` e `block_size` fixos, varia apenas as threads. Observe o FPS subir a cada rodada.

```bash
# 1 thread — baseline sem paralelismo
./mandelbrot 1 256 32

# 2 threads
./mandelbrot 2 256 32

# 4 threads
./mandelbrot 4 256 32

# 8 threads (ou o valor de nproc)
./mandelbrot 8 256 32

# 16 threads (se o CPU tiver núcleos suficientes)
./mandelbrot 16 256 32
```

**O que observar:** o FPS deve crescer quase linearmente até o número de núcleos físicos. Acima disso, o ganho para ou até cai (contenção de contexto).

---

### Teste 2 — Impacto do tamanho do bloco (balanceamento de carga)

Mantém threads e `max_iter` fixos, varia apenas `block_size`. Mais evidente em zoom profundo (quando a fronteira domina a tela).

```bash
# Bloco grande — poucas tarefas, desequilíbrio alto em zoom profundo
./mandelbrot 4 256 128

# Bloco médio-grande
./mandelbrot 4 256 64

# Bloco médio (padrão original)
./mandelbrot 4 256 32

# Bloco pequeno — muitas tarefas, melhor balanceamento
./mandelbrot 4 256 16

# Bloco muito pequeno — overhead de sincronização começa a aparecer
./mandelbrot 4 256 8
```

**O que observar:** em zoom inicial (1×–100×) o FPS é parecido. Em zoom profundo (1000×+) o `block_size=16` deve manter FPS significativamente mais alto que `block_size=64` ou `128`.

---

### Teste 3 — Impacto do max_iter base

Mantém threads e `block_size` fixos, varia o teto de iterações. Afeta diretamente pixels de fronteira.

```bash
# Menos detalhe, mais FPS
./mandelbrot 4 64 32

# Qualidade baixa
./mandelbrot 4 128 32

# Equilíbrio — recomendado
./mandelbrot 4 256 32

# Alta qualidade, FPS mais baixo
./mandelbrot 4 512 32

# Máximo detalhe (pesado)
./mandelbrot 4 1024 32
```

**O que observar:** com `max_iter` baixo (64), o zoom perde detalhe cedo — a fronteira fica "grossa" e sem estrutura. Com `max_iter` alto (1024), o detalhe é máximo mas o FPS cai consideravelmente em zoom profundo.

---

### Teste 4 — Configuração ótima combinada

Com base nos testes anteriores, combine os melhores valores. Exemplos típicos:

```bash
# Balanceado — boa qualidade + FPS estável
./mandelbrot 8 128 16

# Performance máxima — FPS alto, qualidade razoável
./mandelbrot 8 64 16

# Qualidade máxima — FPS pode cair, visual rico em zoom profundo
./mandelbrot 8 512 16

# Para apresentação — zoom suave, visual equilibrado
./mandelbrot 8 256 16
```

---

### Teste 5 — Comparação direta: 1 thread vs N threads

Útil para demonstrar o ganho de paralelismo na apresentação ao professor.

```bash
# Rode cada um por ~30 segundos e compare o FPS médio exibido no título

# Sem paralelismo
./mandelbrot 1 256 16

# Com paralelismo total
./mandelbrot 8 256 16
```

**Resultado esperado:** o FPS com N threads deve ser aproximadamente N× maior que com 1 thread, evidenciando o speedup do pool de Pthreads.

---

### Tabela de referência rápida

| Objetivo | `threads` | `max_iter` | `block_size` |
|----------|-----------|------------|--------------|
| Máximo FPS (benchmark) | `nproc` | `64` | `16` |
| Melhor visual em zoom inicial | `nproc` | `512` | `32` |
| Melhor visual em zoom profundo | `nproc` | `256` | `16` |
| Apresentação equilibrada | `nproc` | `128` | `16` |
| Demonstrar impacto de threads | `1` → `nproc` | `256` | `16` |
| Demonstrar impacto de block_size | `4` | `256` | `128` → `8` |
