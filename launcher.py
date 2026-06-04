"""
launcher.py — Interface gráfica para o Mandelbrot
Configure os parâmetros e clique em Executar.
"""

import os
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

# ── Caminhos ──────────────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def _win_to_wsl(path: str) -> str:
    p = path.replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        p = f"/mnt/{p[0].lower()}{p[2:]}"
    return p

WSL_DIR = _win_to_wsl(SCRIPT_DIR)

# Garante que SDL2 encontra o servidor gráfico do WSLg em qualquer tipo de shell
_WSL_ENV = (
    "export DISPLAY=${DISPLAY:-:0}; "
    "export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}; "
    "export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}; "
)

# ── Dados ─────────────────────────────────────────────────────────────────────
PRESETS = [
    ("Seahorse Valley",  -0.7436438885706799,  0.1318259042053185),
    ("Elephant Valley",   0.3245046418,         0.0485510112),
    ("Triple Spiral",    -0.1011,               0.9563),
    ("Mini Mandelbrot",  -1.7497052,            0.0),
    ("Double Spiral",    -0.7771,               0.1166),
    ("Lightning",        -0.5990,               0.6500),
]

PALETTES = [
    (0, "Padrão",        "#4466ff", "Azul / verde / roxo (canais 120° defasados)"),
    (1, "Fogo",          "#ff6600", "Vermelho → laranja → amarelo"),
    (2, "Oceano",        "#0099dd", "Preto → azul → ciano"),
    (3, "Gold & Purple", "#cc44ff", "Ciclo arco-íris dourado / roxo"),
    (4, "Cinza",         "#aaaaaa", "Escala de cinza pura"),
]

# ── Descrições ────────────────────────────────────────────────────────────────
DESC = {
    "threads":
        "Threads paralelas. Ganho linear até o nº de núcleos físicos da CPU.\n"
        "Recomendado: use o resultado de 'nproc' no terminal WSL.",
    "max_iter":
        "Teto de iterações por pixel. Define o detalhe na fronteira.\n"
        "O programa cresce esse valor automaticamente em zoom profundo.\n"
        "Valores altos = mais detalhe, mais lento por frame.",
    "block_size":
        "Lado do bloco de trabalho em pixels.\n"
        "Menor = granularidade maior = melhor balanceamento entre threads.\n"
        "Use 16 para zoom profundo; 32–64 para zoom inicial.",
    "width":  "Largura da janela SDL2 em pixels.",
    "height": "Altura da janela SDL2 em pixels.",
    "zoom_x":
        "Coordenada X (parte real) do ponto-alvo no plano complexo.\n"
        "⚠ Deve estar na FRONTEIRA do conjunto.\n"
        "Use os presets acima — são pontos já testados e validados.",
    "zoom_y":
        "Coordenada Y (parte imaginária) do ponto-alvo.\n"
        "⚠ Pontos no interior causam tela preta em zoom profundo.",
    "zoom_factor":
        "Fator matemático: f = 1 + p/100. Ex.: 0.8% -> 1.0080.\n"
        "Use a porcentagem para controlar a velocidade do zoom.",
    "frame_delay":
        "Pausa mínima entre frames em milissegundos. Arraste ou digite e pressione Enter.\n"
        "0 = máxima velocidade   16 ≈ 60 FPS   33 ≈ 30 FPS   100 ≈ 10 FPS\n"
        "Valores acima de 500 ms são aceitos digitando no campo.",
}

# ══════════════════════════════════════════════════════════════════════════════

class Launcher:

    # ── Init ──────────────────────────────────────────────────────────────────

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Mandelbrot Launcher")
        self.root.geometry("1000x600")
        self.root.resizable(True, True)
        self.root.configure(bg="#1a1a2e")
        self._proc = None
        self._stop_requested = False

        self._setup_style()
        self._setup_vars()
        self._build_ui()
        self._update_cmd()

    # ── Estilo ────────────────────────────────────────────────────────────────

    def _setup_style(self):
        s = ttk.Style(self.root)
        s.theme_use("clam")

        BG   = "#1a1a2e"
        CARD = "#16213e"
        FLD  = "#0f3460"
        FG   = "#e0e0e0"
        MUTE = "#8888aa"
        ACC  = "#e94560"
        GRN  = "#4ecca3"
        BLU  = "#45b7d1"

        s.configure(".",              background=BG,   foreground=FG,
                    font=("Segoe UI", 10))
        s.configure("TFrame",         background=BG)
        s.configure("Card.TFrame",    background=CARD)
        s.configure("TLabel",         background=BG,   foreground=FG)
        s.configure("Card.TLabel",    background=CARD, foreground=FG)
        s.configure("Mute.TLabel",    background=CARD, foreground=MUTE,
                    font=("Segoe UI", 8, "italic"))
        s.configure("Head.TLabel",    background=BG,   foreground=GRN,
                    font=("Segoe UI", 15, "bold"))
        s.configure("Sub.TLabel",     background=BG,   foreground=MUTE,
                    font=("Segoe UI", 9))
        s.configure("Sec.TLabel",     background=CARD, foreground=BLU,
                    font=("Segoe UI", 9, "bold"))
        s.configure("TLabelframe",    background=CARD, foreground=BLU,
                    relief="flat", borderwidth=1)
        s.configure("TLabelframe.Label", background=CARD, foreground=BLU,
                    font=("Segoe UI", 9, "bold"))
        s.configure("TSpinbox",       fieldbackground=FLD, foreground=FG,
                    background=FLD,  insertcolor=FG,  arrowcolor=GRN)
        s.configure("TEntry",         fieldbackground=FLD, foreground=FG,
                    insertcolor=FG)
        s.configure("TCombobox",      fieldbackground=FLD, foreground=FG,
                    background=FLD,  arrowcolor=GRN,  selectbackground=FLD,
                    selectforeground=FG)
        s.map("TCombobox",            fieldbackground=[("readonly", FLD)])
        s.configure("TRadiobutton",   background=CARD, foreground=FG)
        s.configure("TButton",        background=FLD,  foreground=FG,
                    relief="flat",   padding=(8, 5))
        s.map("TButton",              background=[("active", "#1a4a7a")])
        s.configure("TScale",         background=CARD, troughcolor=FLD,
                    sliderlength=18)

        s.configure("Run.TButton",    background=ACC,  foreground="#ffffff",
                    font=("Segoe UI", 12, "bold"), padding=(22, 10), relief="flat")
        s.map("Run.TButton",          background=[("active", "#c73050")])

        s.configure("Build.TButton",  background="#2d6a4f", foreground=GRN,
                    font=("Segoe UI", 10, "bold"), padding=(12, 6), relief="flat")
        s.map("Build.TButton",        background=[("active", "#1e4d38")])

        s.configure("Stop.TButton",   background="#5a1a1a", foreground="#ff6b6b",
                    font=("Segoe UI", 10, "bold"), padding=(12, 6), relief="flat")
        s.map("Stop.TButton",         background=[("active", "#7a2020")])

        self._CARD = CARD
        self._FLD  = FLD
        self._MUTE = MUTE
        self._GRN  = GRN
        self._ACC  = ACC

    # ── Variáveis ─────────────────────────────────────────────────────────────

    def _setup_vars(self):
        self.var_threads     = tk.IntVar(value=4)
        self.var_max_iter    = tk.IntVar(value=256)
        self.var_block_size  = tk.IntVar(value=32)
        self.var_width       = tk.IntVar(value=700)
        self.var_height      = tk.IntVar(value=500)
        self.var_zoom_x      = tk.StringVar(value="-0.7436438885706799")
        self.var_zoom_y      = tk.StringVar(value="0.1318259042053185")
        self.var_zoom_percent = tk.DoubleVar(value=0.8)
        self.var_frame_delay = tk.IntVar(value=0)
        self.var_palette     = tk.IntVar(value=0)
        self.var_preset      = tk.StringVar(value="Seahorse Valley")
        self.var_status      = tk.StringVar(value="  Pronto")
        self.var_wsl_distro  = tk.StringVar(value="Ubuntu")
        self.var_wsl_path    = tk.StringVar(value=WSL_DIR)
        self.var_mode        = tk.StringVar(value="wsl")
        self.var_win_path    = tk.StringVar(value=SCRIPT_DIR)

        for v in (self.var_threads, self.var_max_iter, self.var_block_size,
                  self.var_width, self.var_height, self.var_zoom_x,
                self.var_zoom_y, self.var_zoom_percent, self.var_frame_delay,
                  self.var_palette, self.var_wsl_distro, self.var_wsl_path,
                  self.var_win_path):
            v.trace_add("write", lambda *_: self._update_cmd())

    # ── UI ────────────────────────────────────────────────────────────────────

    def _build_ui(self):
        R = self.root
        BG = "#1a1a2e"
        CARD = self._CARD

        # ── Área rolável ────────────────────────────────────────────────────
        scroll_outer = tk.Frame(R, bg=BG)
        scroll_outer.pack(fill="both", expand=True)

        scroll_canvas = tk.Canvas(scroll_outer, bg=BG, highlightthickness=0, bd=0)
        scroll_canvas.pack(side="left", fill="both", expand=True)

        scroll_bar = ttk.Scrollbar(scroll_outer, orient="vertical",
                                   command=scroll_canvas.yview)
        scroll_bar.pack(side="right", fill="y")

        scroll_canvas.configure(yscrollcommand=scroll_bar.set)

        content = tk.Frame(scroll_canvas, bg=BG)
        content_id = scroll_canvas.create_window((0, 0), window=content, anchor="nw")

        def _sync_scrollregion(_=None):
            scroll_canvas.configure(scrollregion=scroll_canvas.bbox("all"))

        def _sync_content_width(event):
            scroll_canvas.itemconfigure(content_id, width=event.width)

        def _on_mousewheel(event):
            if event.delta:
                scroll_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
                return "break"

        def _on_linux_scroll(event):
            if event.num == 4:
                scroll_canvas.yview_scroll(-3, "units")
            elif event.num == 5:
                scroll_canvas.yview_scroll(3, "units")
            return "break"

        def _bind_mousewheel(_=None):
            scroll_canvas.bind_all("<MouseWheel>", _on_mousewheel)
            scroll_canvas.bind_all("<Button-4>", _on_linux_scroll)
            scroll_canvas.bind_all("<Button-5>", _on_linux_scroll)

        def _unbind_mousewheel(_=None):
            scroll_canvas.unbind_all("<MouseWheel>")
            scroll_canvas.unbind_all("<Button-4>")
            scroll_canvas.unbind_all("<Button-5>")

        content.bind("<Configure>", _sync_scrollregion)
        scroll_canvas.bind("<Configure>", _sync_content_width)
        scroll_canvas.bind("<Enter>", _bind_mousewheel)
        scroll_canvas.bind("<Leave>", _unbind_mousewheel)

        R = content

        # ── Cabeçalho ─────────────────────────────────────────────────────────
        hdr = tk.Frame(R, bg="#1a1a2e")
        hdr.pack(fill="x", padx=16, pady=(14, 8))
        tk.Label(hdr, text="Mandelbrot Launcher",
                 bg="#1a1a2e", fg=self._GRN,
                 font=("Segoe UI", 16, "bold")).pack(side="left")
        tk.Label(hdr, text="  Configure e clique em Executar",
                 bg="#1a1a2e", fg=self._MUTE,
                 font=("Segoe UI", 9)).pack(side="left", pady=(4, 0))

        # ── Ambiente de execução (full-width) ────────────────────────────────
        env_outer = tk.Frame(R, bg=CARD, bd=0)
        env_outer.pack(fill="x", padx=10, pady=(0, 8))
        tk.Label(env_outer, text="AMBIENTE DE EXECUÇÃO", bg=CARD, fg="#45b7d1",
                 font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=10, pady=(8, 4))
        env_inner = tk.Frame(env_outer, bg=CARD)
        env_inner.pack(fill="x", padx=10, pady=(0, 10))

        # Seletor de modo
        mode_row = tk.Frame(env_inner, bg=CARD)
        mode_row.pack(fill="x", pady=(0, 8))
        tk.Label(mode_row, text="Modo:", bg=CARD, fg="#e0e0e0",
                 font=("Segoe UI", 9, "bold")).pack(side="left", padx=(0, 10))
        ttk.Radiobutton(mode_row, text="WSL (Linux)",
                        variable=self.var_mode, value="wsl",
                        command=self._on_mode_change).pack(side="left", padx=(0, 16))
        ttk.Radiobutton(mode_row, text="Windows Nativo",
                        variable=self.var_mode, value="windows",
                        command=self._on_mode_change).pack(side="left")

        # ── Campos WSL ────────────────────────────────────────────────────────
        self._wsl_fields = tk.Frame(env_inner, bg=CARD)
        self._wsl_fields.pack(fill="x")
        self._wsl_fields.columnconfigure(1, weight=1)
        self._wsl_fields.columnconfigure(3, weight=4)

        tk.Label(self._wsl_fields, text="Distribuição WSL", bg=CARD, fg="#e0e0e0",
                 font=("Segoe UI", 9, "bold"), anchor="w"
                 ).grid(row=0, column=0, sticky="w", padx=(0, 8))
        ttk.Entry(self._wsl_fields, textvariable=self.var_wsl_distro,
                  font=("Cascadia Code", 10), width=14
                  ).grid(row=0, column=1, sticky="ew", padx=(0, 24))
        tk.Label(self._wsl_fields, text="Caminho do projeto (WSL)", bg=CARD, fg="#e0e0e0",
                 font=("Segoe UI", 9, "bold"), anchor="w"
                 ).grid(row=0, column=2, sticky="w", padx=(0, 8))
        wsl_path_row = tk.Frame(self._wsl_fields, bg=CARD)
        wsl_path_row.grid(row=0, column=3, sticky="ew")
        wsl_path_row.columnconfigure(0, weight=1)
        ttk.Entry(wsl_path_row, textvariable=self.var_wsl_path,
                  font=("Cascadia Code", 10)
                  ).grid(row=0, column=0, sticky="ew", padx=(0, 6))
        ttk.Button(wsl_path_row, text="↺",
                   command=lambda: self.var_wsl_path.set(WSL_DIR),
                   width=3).grid(row=0, column=1)
        tk.Label(self._wsl_fields,
                 text="Distribuição: nome exato (wsl --list).  "
                      "Caminho: pasta do projeto em notação Linux (/mnt/c/...).  "
                      "↺ restaura o caminho detectado automaticamente.",
                 bg=CARD, fg=self._MUTE, font=("Segoe UI", 8, "italic"),
                 wraplength=800, justify="left"
                 ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(6, 0))

        # ── Campos Windows (oculto por padrão) ───────────────────────────────
        self._win_fields = tk.Frame(env_inner, bg=CARD)
        # não empacotado ainda — aparece ao trocar para modo Windows
        self._win_fields.columnconfigure(1, weight=1)

        tk.Label(self._win_fields, text="Caminho do projeto (Windows)", bg=CARD,
                 fg="#e0e0e0", font=("Segoe UI", 9, "bold"), anchor="w"
                 ).grid(row=0, column=0, sticky="w", padx=(0, 8))
        win_path_row = tk.Frame(self._win_fields, bg=CARD)
        win_path_row.grid(row=0, column=1, sticky="ew")
        win_path_row.columnconfigure(0, weight=1)
        ttk.Entry(win_path_row, textvariable=self.var_win_path,
                  font=("Cascadia Code", 10)
                  ).grid(row=0, column=0, sticky="ew", padx=(0, 6))
        ttk.Button(win_path_row, text="↺",
                   command=lambda: self.var_win_path.set(SCRIPT_DIR),
                   width=3).grid(row=0, column=1)
        tk.Label(self._win_fields,
                 text="Pasta onde estão main.cpp e main.exe.  "
                      "Compilação: g++ main.cpp -O2 -std=c++17 -o main.exe -lmingw32 -lSDL2main -lSDL2  "
                      "↺ restaura o caminho da pasta do launcher.",
                 bg=CARD, fg=self._MUTE, font=("Segoe UI", 8, "italic"),
                 wraplength=800, justify="left"
                 ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))

        # ── Corpo: duas colunas ───────────────────────────────────────────────
        body = tk.Frame(R, bg="#1a1a2e")
        body.pack(fill="both", expand=True, padx=10, pady=0)
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)

        left  = tk.Frame(body, bg="#1a1a2e")
        right = tk.Frame(body, bg="#1a1a2e")
        left.grid (row=0, column=0, sticky="nsew", padx=(0, 5))
        right.grid(row=0, column=1, sticky="nsew", padx=(5, 0))

        # ════ COLUNA ESQUERDA ═════════════════════════════════════════════════

        # ── Desempenho ────────────────────────────────────────────────────────
        self._card(left, "DESEMPENHO", [
            ("Threads",            self.var_threads,    "threads",    (1,  32,  1)),
            ("Máx. Iterações",     self.var_max_iter,   "max_iter",   (32, 4096, 32)),
            ("Tamanho do Bloco",   self.var_block_size, "block_size", (4,  128,  4)),
        ])

        # ── Janela ────────────────────────────────────────────────────────────
        self._card(left, "JANELA", [
            ("Largura (px)",  self.var_width,  "width",  (200, 3840, 100)),
            ("Altura  (px)",  self.var_height, "height", (200, 2160, 100)),
        ])

        # ── Paleta ────────────────────────────────────────────────────────────
        pal_card = self._card_frame(left, "PALETA DE CORES")
        for idx, name, color, tip in PALETTES:
            row = tk.Frame(pal_card, bg=CARD)
            row.pack(fill="x", pady=2)
            # Swatch de cor
            sw = tk.Canvas(row, width=16, height=16, bg=CARD,
                           highlightthickness=0)
            sw.pack(side="left", padx=(0, 6))
            sw.create_oval(2, 2, 14, 14, fill=color, outline="")
            rb = ttk.Radiobutton(row, text=name,
                                 variable=self.var_palette, value=idx)
            rb.pack(side="left")
            tk.Label(row, text=f"  {tip}", bg=CARD, fg=self._MUTE,
                     font=("Segoe UI", 8, "italic")).pack(side="left")

        # ════ COLUNA DIREITA ══════════════════════════════════════════════════

        # ── Ponto de zoom ─────────────────────────────────────────────────────
        zoom_card = self._card_frame(right, "PONTO ALVO DO ZOOM")

        # Preset dropdown
        tk.Label(zoom_card, text="Preset", bg=CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "bold")).pack(anchor="w")
        combo = ttk.Combobox(zoom_card, textvariable=self.var_preset,
                             values=[p[0] for p in PRESETS],
                             state="readonly", font=("Segoe UI", 10))
        combo.pack(fill="x", pady=(2, 0))
        combo.bind("<<ComboboxSelected>>", self._on_preset)
        tk.Label(zoom_card,
                 text="Selecione um ponto predefinido para preencher X e Y automaticamente",
                 bg=CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "italic"),
                 wraplength=300, justify="left").pack(anchor="w", pady=(2, 8))

        # X e Y
        for lbl, var, key in (("X (parte real)", self.var_zoom_x, "zoom_x"),
                               ("Y (parte imaginária)", self.var_zoom_y, "zoom_y")):
            tk.Label(zoom_card, text=lbl, bg=CARD, fg=self._MUTE,
                     font=("Segoe UI", 8, "bold")).pack(anchor="w", pady=(6, 0))
            ttk.Entry(zoom_card, textvariable=var,
                      font=("Cascadia Code", 10)).pack(fill="x", pady=(2, 0))

        tk.Label(zoom_card, text=DESC["zoom_x"],
                 bg=CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "italic"),
                 wraplength=300, justify="left").pack(anchor="w", pady=(4, 0))

        # ── Velocidade ────────────────────────────────────────────────────────
        self._card_slider(right, "VELOCIDADE DO ZOOM (% por frame)",
                  self.var_zoom_percent, 0.1, 5.0, 0.1,
                  "zoom_factor", fmt="{:.1f}")
        self._card_slider(right, "DELAY ENTRE FRAMES (ms)",
                          self.var_frame_delay, 0, 500, 1,
                          "frame_delay", fmt="{:.0f}")

        # ── Preview do comando ────────────────────────────────────────────────
        sep_frame = tk.Frame(R, bg="#1a1a2e")
        sep_frame.pack(fill="x", padx=10, pady=(8, 4))
        tk.Frame(sep_frame, bg="#333355", height=1).pack(fill="x")

        cmd_outer = tk.Frame(R, bg=CARD)
        cmd_outer.pack(fill="x", padx=10, pady=(0, 4))
        tk.Label(cmd_outer, text="Comando", bg=CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "bold")).pack(anchor="w", padx=8, pady=(6, 2))
        self.cmd_text = tk.Text(cmd_outer, height=2, wrap="word",
                                bg="#0a0a1a", fg="#4ecca3",
                                font=("Cascadia Code", 8),
                                relief="flat", state="disabled",
                                insertbackground="#4ecca3",
                                padx=8, pady=6)
        self.cmd_text.pack(fill="x", padx=8, pady=(0, 8))

        # ── Log de saída ──────────────────────────────────────────────────────
        log_outer = tk.Frame(R, bg=CARD)
        log_outer.pack(fill="both", expand=True, padx=10, pady=(0, 4))
        tk.Label(log_outer, text="Saída do programa", bg=CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "bold")).pack(anchor="w", padx=8, pady=(6, 2))
        self.log = scrolledtext.ScrolledText(
            log_outer, height=5, bg="#0a0a1a", fg="#cccccc",
            font=("Cascadia Code", 8), relief="flat",
            state="disabled", padx=8, pady=6
        )
        self.log.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        # ── Rodapé ────────────────────────────────────────────────────────────
        footer = tk.Frame(R, bg="#1a1a2e")
        footer.pack(fill="x", padx=10, pady=(0, 12))

        ttk.Button(footer, text="  Compilar  (make)",
                   style="Build.TButton",
                   command=self._compile).pack(side="left", padx=(0, 8))

        self.stop_btn = ttk.Button(footer, text="  Parar",
                                   style="Stop.TButton",
                                   command=self._stop, state="disabled")
        self.stop_btn.pack(side="left")

        # Status
        status_frame = tk.Frame(footer, bg="#1a1a2e")
        status_frame.pack(side="left", padx=12)
        self.status_dot = tk.Label(status_frame, text="●", bg="#1a1a2e",
                                   fg="#4ecca3", font=("Segoe UI", 11))
        self.status_dot.pack(side="left")
        tk.Label(status_frame, textvariable=self.var_status,
                 bg="#1a1a2e", fg=self._MUTE,
                 font=("Segoe UI", 9)).pack(side="left")

        ttk.Button(footer, text="  ▶  EXECUTAR  ",
                   style="Run.TButton",
                   command=self._run).pack(side="right")

    # ── Helpers de construção ─────────────────────────────────────────────────

    def _card_frame(self, parent, title: str) -> tk.Frame:
        """Retorna o frame interno de um card com título."""
        outer = tk.Frame(parent, bg=self._CARD, bd=0)
        outer.pack(fill="x", pady=(0, 8))
        tk.Label(outer, text=title, bg=self._CARD, fg="#45b7d1",
                 font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=10, pady=(8, 4))
        inner = tk.Frame(outer, bg=self._CARD)
        inner.pack(fill="x", padx=10, pady=(0, 10))
        return inner

    def _card(self, parent, title: str, params: list):
        """Cria um card com N spinboxes."""
        inner = self._card_frame(parent, title)
        for label, var, key, (lo, hi, inc) in params:
            self._spinbox_row(inner, label, var, key, lo, hi, inc)

    def _spinbox_row(self, parent, label: str, var, key: str,
                     lo: int, hi: int, inc: int):
        row = tk.Frame(parent, bg=self._CARD)
        row.pack(fill="x", pady=(0, 8))
        row.columnconfigure(1, weight=1)

        tk.Label(row, text=label, bg=self._CARD, fg="#e0e0e0",
                 font=("Segoe UI", 9, "bold"),
                 width=20, anchor="w").grid(row=0, column=0, sticky="w")

        sb = ttk.Spinbox(row, from_=lo, to=hi, increment=inc,
                         textvariable=var,
                         font=("Cascadia Code", 10), width=10)
        sb.grid(row=0, column=1, sticky="e")

        tk.Label(row, text=DESC[key], bg=self._CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "italic"),
                 wraplength=310, justify="left",
                 anchor="w").grid(row=1, column=0, columnspan=2,
                                  sticky="w", pady=(2, 0))

    def _card_slider(self, parent, title: str, var, lo, hi, res: float,
                     key: str, fmt="{:.0f}"):
        inner = self._card_frame(parent, title)
        row = tk.Frame(inner, bg=self._CARD)
        row.pack(fill="x")

        # Entry editável — substitui o label fixo
        # Permite digitar qualquer valor diretamente (Enter ou Tab para confirmar)
        entry_var = tk.StringVar(value=fmt.format(var.get()))
        entry = ttk.Entry(row, textvariable=entry_var, width=9,
                          font=("Cascadia Code", 10), justify="right")
        entry.pack(side="right")

        scale = ttk.Scale(row, orient="horizontal", from_=lo, to=hi, variable=var)
        scale.pack(side="left", fill="x", expand=True, padx=(0, 8))

        # Slider/var → Entry: atualiza o campo sempre que var muda
        def _var_to_entry(*_):
            try:
                entry_var.set(fmt.format(var.get()))
            except Exception:
                pass

        # Entry → Var: aplica o valor digitado ao confirmar (Enter / Tab / clique fora)
        def _entry_to_var(_=None):
            try:
                val = float(entry_var.get().strip().replace(",", "."))
                val = max(lo, val)           # respeita o mínimo
                if res < 1:
                    val = round(round(val / res) * res, 6)
                else:
                    val = int(round(val))
                var.set(val)
            except Exception:
                _var_to_entry()              # restaura o último valor válido

        def _refresh(_=None):
            raw = var.get()
            if res < 1:
                rounded = round(raw / res) * res
                var.set(round(rounded, 6))

        scale.bind("<Motion>",        _refresh)
        scale.bind("<ButtonRelease>", _refresh)
        var.trace_add("write",  _var_to_entry)
        entry.bind("<Return>",  _entry_to_var)
        entry.bind("<FocusOut>", _entry_to_var)

        tk.Label(inner, text=DESC[key], bg=self._CARD, fg=self._MUTE,
                 font=("Segoe UI", 8, "italic"),
                 wraplength=310, justify="left",
                 anchor="w").pack(anchor="w", pady=(4, 0))

    # ── Helpers de ambiente ───────────────────────────────────────────────────

    def _distro(self) -> str:
        return self.var_wsl_distro.get().strip() or "Ubuntu"

    def _wsldir(self) -> str:
        return self.var_wsl_path.get().strip() or WSL_DIR

    def _windir(self) -> str:
        return self.var_win_path.get().strip() or SCRIPT_DIR

    def _wsl_argv(self, bash_cmd: str) -> list:
        return ["wsl", "-d", self._distro(), "-e", "bash", "-c", bash_cmd]

    def _on_mode_change(self):
        if self.var_mode.get() == "wsl":
            self._win_fields.pack_forget()
            self._wsl_fields.pack(fill="x")
        else:
            self._wsl_fields.pack_forget()
            self._win_fields.pack(fill="x")
        self._update_cmd()

    # ── Lógica ────────────────────────────────────────────────────────────────

    def _on_preset(self, _event=None):
        name = self.var_preset.get()
        for n, x, y in PRESETS:
            if n == name:
                self.var_zoom_x.set(str(x))
                self.var_zoom_y.set(str(y))
                break

    def _build_args(self) -> list:
        def si(var, default):
            try:
                return str(int(var.get()))
            except Exception:
                return str(default)

        def sf(var, fmt, default):
            try:
                return fmt.format(float(var.get()))
            except Exception:
                return fmt.format(default)

        def ss(var, default):
            try:
                v = str(var.get()).strip()
                float(v)  # valida que é número
                return v
            except Exception:
                return str(default)

        def zoom_factor_from_percent(default=1.008):
            try:
                percent = float(self.var_zoom_percent.get())
                return f"{1.0 + (percent / 100.0):.4f}"
            except Exception:
                return f"{default:.4f}"

        return [
            "--threads",     si(self.var_threads,    4),
            "--max-iter",    si(self.var_max_iter,   256),
            "--block-size",  si(self.var_block_size, 32),
            "--width",       si(self.var_width,      900),
            "--height",      si(self.var_height,     900),
            "--zoom-x",      ss(self.var_zoom_x,     -0.7436438885706799),
            "--zoom-y",      ss(self.var_zoom_y,      0.1318259042053185),
            # Converte a porcentagem escolhida no fator multiplicativo usado pelo executável.
            "--zoom-factor", zoom_factor_from_percent(),
            "--frame-delay", si(self.var_frame_delay, 0),
            "--palette",     si(self.var_palette,     0),
        ]

    def _update_cmd(self):
        try:
            args = self._build_args()
            if self.var_mode.get() == "wsl":
                bash = f"cd '{self._wsldir()}' && ./mandelbrot " + " ".join(args)
                full = f'wsl -d {self._distro()} -e bash -c "{bash}"'
            else:
                full = f'.\\main.exe ' + " ".join(args)
            self.cmd_text.config(state="normal")
            self.cmd_text.delete("1.0", "end")
            self.cmd_text.insert("1.0", full)
            self.cmd_text.config(state="disabled")
        except Exception:
            pass

    def _bash_cmd(self, args: list) -> str:
        """Monta o comando bash completo com env do WSLg garantida."""
        run = "./mandelbrot " + " ".join(args)
        return f"cd '{self._wsldir()}' && {_WSL_ENV}{run}"

    def _log(self, msg: str, color="#cccccc"):
        def _do():
            self.log.config(state="normal")
            self.log.insert("end", msg + "\n")
            self.log.see("end")
            self.log.config(state="disabled")
        self.root.after(0, _do)

    def _set_status(self, text: str, dot_color: str):
        self.root.after(0, lambda: (
            self.var_status.set(f"  {text}"),
            self.status_dot.config(fg=dot_color)
        ))

    def _validate(self) -> bool:
        try:
            float(self.var_zoom_x.get())
            float(self.var_zoom_y.get())
        except ValueError:
            messagebox.showerror("Erro",
                "Ponto Alvo X e Y devem ser números decimais (use '.' como separador).")
            return False
        if self.var_zoom_percent.get() <= 0.0:
            messagebox.showerror("Erro",
                "Velocidade do Zoom deve ser maior que 0%.")
            return False
        return True

    # ── Executar ──────────────────────────────────────────────────────────────

    def _run(self):
        if self._proc is not None:
            messagebox.showinfo(
                "Em execução",
                "Um processo já está rodando.\nClique em Parar antes de executar novamente."
            )
            return

        if not self._validate():
            return

        self._set_status("Preparando compilação…", "#f9ca24")

        try:
            if self.var_mode.get() == "windows":
                exe_path = os.path.join(self._windir(), "main.exe")

                if os.path.exists(exe_path):
                    try:
                        os.remove(exe_path)
                        self._log("main.exe removido.", "#f9ca24")
                    except Exception as e:
                        self._log(f"Falha ao remover main.exe: {e}", "#e94560")
                        return

        except Exception as e:
            self._log(f"Erro ao preparar compilação: {e}", "#e94560")
            return

        self._compile(then_run=True)

    def _do_run(self):
        try:
            args = self._build_args()
        except Exception as e:
            self._log(f"Erro nos parâmetros: {e}", "#e94560")
            messagebox.showerror("Parâmetros inválidos",
                f"Não foi possível ler os parâmetros da interface.\n\n{e}")
            return

        if self.var_mode.get() == "wsl":
            bash_cmd = self._bash_cmd(args)
            argv     = self._wsl_argv(bash_cmd)
            cwd      = None
            self._log("$ ./mandelbrot " + " ".join(args), "#45b7d1")
        else:
            import os
            exe  = os.path.join(self._windir(), "main.exe")
            argv = [exe] + args
            cwd  = self._windir()
            self._log(f"$ .\\main.exe " + " ".join(args), "#45b7d1")

        self._set_status("Executando…", "#f9ca24")
        self.stop_btn.config(state="normal")
        self._stop_requested = False

        def _worker():
            try:
                self._proc = subprocess.Popen(
                    argv,
                    cwd=cwd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                for line in self._proc.stdout:
                    self._log(line.rstrip())
                self._proc.wait()
                code = self._proc.returncode
                if self._stop_requested:
                    self._log("Processo encerrado após interrupção do usuário.", "#f9ca24")
                    self._set_status("Parado", "#f9ca24")
                    return
                if code == 0:
                    self._log("Processo encerrado.", "#4ecca3")
                    self._set_status("Encerrado", "#4ecca3")
                else:
                    self._log(f"Processo terminou com código {code}.", "#e94560")
                    self._set_status(f"Erro (código {code})", "#e94560")
            except FileNotFoundError:
                if self.var_mode.get() == "wsl":
                    self._log("Erro: 'wsl' não encontrado. Verifique se o WSL2 está instalado.", "#e94560")
                    self._set_status("Erro: WSL não encontrado", "#e94560")
                else:
                    self._log("Erro: 'main.exe' não encontrado. Verifique o caminho do projeto.", "#e94560")
                    self._set_status("Erro: executável não encontrado", "#e94560")
            finally:
                self._proc = None
                self.root.after(0, lambda: self.stop_btn.config(state="disabled"))

        threading.Thread(target=_worker, daemon=True).start()

    def _stop(self):
        if self._proc:
            try:
                self._stop_requested = True
                if self.var_mode.get() == "wsl":
                    # Mata o processo mandelbrot diretamente no Linux antes de
                    # matar o wsl.exe — evita que ele fique zumbi no WSL
                    subprocess.Popen(
                        self._wsl_argv("pkill -TERM -x mandelbrot 2>/dev/null; true"),
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                    )
                self._proc.terminate()
                self._log("Processo interrompido pelo usuário.", "#e94560")
                self._set_status("Parado", "#f9ca24")
            except Exception as e:
                self._log(f"Erro ao parar: {e}", "#e94560")

    # ── Compilar ──────────────────────────────────────────────────────────────

    def _compile(self, then_run=False):
        if self.var_mode.get() == "wsl":
            bash_cmd = f"cd '{self._wsldir()}' && {_WSL_ENV}make 2>&1"
            argv = self._wsl_argv(bash_cmd)
            cwd  = None
            self._log("Compilando com make (WSL)…", "#45b7d1")
        else:
            argv = [
                "g++", "main.cpp",
                "-O2", "-std=c++17", "-Wall", "-Wextra", "-march=native",
                "-o", "main.exe",
                "-lmingw32", "-lSDL2main", "-lSDL2",
            ]
            cwd = self._windir()
            self._log("Compilando com g++ (Windows)…", "#45b7d1")

        self._set_status("Compilando…", "#f9ca24")

        def _worker():
            try:
                proc = subprocess.Popen(
                    argv, cwd=cwd,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                )
                for line in proc.stdout:
                    self._log(line.rstrip())
                proc.wait()
                if proc.returncode == 0:
                    self._log("Compilação concluída com sucesso!", "#4ecca3")
                    self._set_status("Compilado", "#4ecca3")
                    if then_run:
                        self.root.after(200, self._do_run)
                else:
                    self._log(f" Falha na compilação (código {proc.returncode}).", "#e94560")
                    self._set_status("Falha na compilação", "#e94560")
            except FileNotFoundError:
                tool = "wsl" if self.var_mode.get() == "wsl" else "g++"
                self._log(f"Erro: '{tool}' não encontrado no PATH.", "#e94560")

        threading.Thread(target=_worker, daemon=True).start()

    # ── Loop ──────────────────────────────────────────────────────────────────

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    Launcher().run()
