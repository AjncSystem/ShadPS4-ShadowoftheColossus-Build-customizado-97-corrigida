# ShadPS4 · Shadow of the Colossus — custom build (97% fixed) · build customizado · SotC 0.2.7

**🇺🇸 English** · [Português abaixo](#portugues)

> [!IMPORTANT]
> **To play, download SotC 0.2.7 from [Releases](../../releases)** (`ShadPS4-SotC-0.2.7.rar`). The green **Code → Download ZIP** button only gets the source code, not the built emulator. Do not use the shadPS4 the Launcher downloads by itself (official/Nightly): it lacks these fixes. **Sound:** the game needs 2 files from your own PS4 firmware (see "How to use", step 2); without them it is silent.
>
> **Para jogar, baixe o SotC 0.2.7 em [Releases](../../releases)** (`ShadPS4-SotC-0.2.7.rar`). O botão verde **Code → Download ZIP** traz só o código-fonte, sem o emulador pronto. Não use o shadPS4 que o Launcher baixa sozinho (oficial/Nightly): ele não tem estas correções. **Som:** o jogo precisa de 2 arquivos do firmware do seu PS4 (veja "Como usar", passo 2); sem eles fica mudo.

**Unofficial** build of the [shadPS4](https://github.com/shadps4-emu/shadPS4) emulator with fixes for **Shadow of the Colossus** (PS4, CUSA08809 EU, v1.01) on **Windows**. It is not distributed by the shadPS4 team. **No game files are included**: you need your own dump.

### Download
From the **[Releases](../../releases)** tab, get `ShadPS4-SotC-0.2.7.rar` — version **SotC 0.2.7**, shown in the emulator window title (the "Source code" entries there are only the code).

### What is fixed
- No GPU hang in the intro and no crashes when skipping the intro video or loading the save.
- Menu text, the temple light shafts, no black blocks, no rectangles on the ground.
- **Bird colossus lake:** no cross-shaped white flashes or flickering white dots on the horizon while the boss flies.
- Correct auto-exposure on NVIDIA GPUs; less motion-blur ghosting and a less blown-out sun.
- 17–18 fps in the open world (was 13–15).

Technical details of every fix (cause and solution): [`documents/SotC-RELATORIO-REPORT.html`](documents/SotC-RELATORIO-REPORT.html) (download and open in a browser; PT and EN).

### Full list of fixes
Every item has its cause, fix and commits in the [technical report](documents/SotC-RELATORIO-REPORT.html). ✅ = upstream candidate (generic); 🎮 = SotC-specific (enabled only for `CUSA08809`).

**Core and Windows**
| # | Fix | What it solved |
|---|---|---|
| 01 | ✅ Short SSE4a instructions relocated | crash around 35 s on Intel CPUs (red zone wiped) |
| 02 | ✅ Fault handler on an alternate stack | game closing with no log |
| 03 | ✅ Thread/fiber stacks never protected | crashes when loading the save |
| 04 | ✅ Page protections reapplied after SplitRegion | stale GPU data, GPU hangs (TDR) |
| 05 | 🎮 Selective red-zone protection | crashes in 3 game functions |

**Vulkan and synchronization**
| # | Fix | What it solved |
|---|---|---|
| 06 | ✅ A stage mask for every wait semaphore | stalls/deadlocks |
| 07 | ✅ Device loss does not freeze the emulator | emulator stuck forever |
| 08 | ✅ Depth copies between formats through a buffer | crash when skipping the intro video |
| 09 | ✅ Stages no longer leak between cached pipelines | crash in the first frames |
| 10 | ✅ Coherence of indirect/vertex reads and fences | GPU using stale data |
| 11 | ✅ Running out of video memory does not abort | crash on 8 GB cards |
| 12 | ✅ No deadlock between the fault handler and textures | freeze |
| 13 | ✅ Larger SRT walker buffer | boot crash with a large cache |
| 27 | ✅ Stack pages tracked as whole pages; GPU read-backs never write over them | random crashes after a few minutes (game heap corrupted) |
| 28 | ✅ GPU read-back across several memory mappings (upstream bug) | corrupted game memory |
| 29 | ✅ Buffers at unmapped (garbage) addresses bound as null | crash in the opening cutscene (video memory exhausted) |

**Shader recompiler**
| # | Fix | What it solved |
|---|---|---|
| 14 | ✅ Wave64 ReadLane on 32-wide subgroup GPUs | wrong reductions on NVIDIA |
| 15 | 🎮 Wave64 uniform branches and LDS barriers | lighting stripes |
| 16 | 🎮 Depth tests before the pixel shader | dark rectangles on the ground |
| 17 | ✅ Runtime-indexed ReadConst | black blocks (depth of field) |
| 18 | ✅ Others (V_MAD_LEGACY, garbage descriptors, loop budget…) | asserts and hangs |
| 24 | ✅ Negated compares with NaN | temple light shafts wiped out |
| 25 | ✅ Instance ID relative to the draw start | white flashes at the bird lake |
| 26 | 🎮 Subgroup barriers in LDS reductions | wrong auto-exposure |

**Performance** (13–15 → 17–18 fps)
| # | Fix |
|---|---|
| 19 | 🎮 Periodic command submission (the biggest win) |
| 20 | ✅ Batched readbacks |
| 21 | ✅ Clean SRT walker reads |
| 22 | ✅ Readback ahead |

**Image**
| # | Fix |
|---|---|
| 23 | 🎮 Shader patch: less motion-blur "ghosting" and a 20% less blown-out sun |

### Tools and switches
Environment variables (set them before starting `shadPS4.exe`, e.g. in a `.bat` with `set NAME=value`). The defaults are the recommended ones; change them only for testing.

| Variable | Default | Purpose |
|---|---|---|
| `SOTC_FLUSH_EVERY` | 256 for SotC on NVIDIA, 0 on AMD/Intel | periodic command submission; `0` disables |
| `SOTC_RB_HOT` | 2 | batched readbacks (0 off, 1 conservative, 2 any arena) |
| `SOTC_CLEAN_READS` | 1 | clean SRT walker reads |
| `SOTC_RB_AHEAD` | 1 on NVIDIA, 0 on AMD/Intel | readback ahead |
| `SOTC_EARLY_Z` | 1 for SotC | early depth tests |
| `SOTC_WAVE64_UNIFORM` | 1 for SotC | wave64 uniform branches and LDS barriers |
| `SOTC_LDS_BARRIERS` | 1 for SotC | subgroup barriers in multi-wave workgroups |
| `SOTC_ARENA_RELEASE` | off | release arena memory on unmap (experimental) |
| `SHADPS4_LOOP_LIMIT` | 8192 for SotC | maximum loop iterations per shader |
| `SHADPS4_REDZONE_PROTECT` | — | extra functions for red-zone protection |
| `SHADPS4_IEEE_MINMAX` | off | restores the old min/max/clamp |
| `SHADPS4_ABSOLUTE_INSTANCE_ID` | off | restores the old instance ID |

**Diagnostic tools** (used to find the bugs; off by default):
- `SOTC_FRAME_LOG=N` — prints the frame count and time every N frames (measures fps).
- `SOTC_WRITE_RING=1` — records the emulator's writes into game memory and, if the game crashes, logs which of them landed next to the crash (`SOTCRING` lines).
- `SOTC_PROBE="0xADDRESS:N,…"` (+ `SOTC_PROBE_MS`) — sensor: logs values the GPU computes while playing (e.g. auto-exposure) to `user/log/sotc_probe.txt`.
- `SOTC_DUMP=1` — with RenderDoc off, **F12** saves the real images of every volumetric fog pass to `user/log/dump_N/` (this found the lake bug).
- `SOTC_OCCLUSION_STEP` — overrides the fake occlusion query counter (A/B tests).
- [`sotc/shader_patch/make_patch.py`](sotc/shader_patch) — regenerates the image patch (motion blur/sun) when the recompiler changes.

### Modes (Cheats / Patches menu)
The package brings `user\patches\SotC-Modes` with **both modes already enabled**. To turn one off: in the launcher, right-click the game → **Cheats / Patches** → **Patches** tab → `SotC-Modes.xml`, untick it and click **Save**. Source: [`sotc/patches/SotC-Modes`](sotc/patches/SotC-Modes).

| Mode | What it does | Measured (bird lake, RTX 2060 SUPER) |
|---|---|---|
| Sharp textures (16x AF) | 16x anisotropic filtering on every texture: ground, grass and rocks stay sharp at a distance | no fps cost (29.5 vs 28.5) |
| 60 FPS | removes the game's 30 fps cap (illusion's GoldHEN patch) | 33 fps average, up to ~46 |

Both can be combined. Do not enable *Sharp textures* together with the shadPS4 repository's *Custom Debug Menu Config* (they write to the same place). Not possible for now: 1440p/2160p (the game only renders them in PS4 Pro mode, which still crashes at boot in shadPS4) and 21:9/19:9 (the game has no resolution or aspect ratio setting to change).

### How to use
1. Extract the rar somewhere with free space (not "Program Files").
2. **Audio — without this the game is SILENT:** copy `libSceNgs2.sprx` and `libSceUlt.sprx` from **your own** PS4 firmware into `user\sys_modules\` (Sony firmware, it cannot be distributed here). With GoldHEN: enable FTP in the GoldHEN settings, connect from the PC (e.g. FileZilla) to the PS4's IP, port `2121`, open `/system/common/lib/` and copy both files. If you already use shadPS4 with other games, copy them from your `sys_modules` folder.
3. Open the folder's `shadPS4QtLauncher.exe`: it comes preconfigured with this emulator (no automatic updates). The first time, pick your games folder, then double-click the game. You can also drag `eboot.bin` onto `Iniciar-SotC.bat`.
4. The first start compiles the shaders and takes longer.

Tested on Windows 11, Intel i9-13900K and NVIDIA RTX 2060 SUPER 8 GB. Not tested on AMD/Intel GPUs, Linux or macOS.

### Source code
This repository is shadPS4 itself (base `259e815a`) with the fixes, commit by commit; the original shadPS4 README is in [`README.shadPS4.md`](README.shadPS4.md). Building: same as shadPS4 ([`documents/building-windows.md`](documents/building-windows.md)). Generic fixes are marked in the report as upstream candidates.

---

<a id="portugues"></a>
**🇧🇷 Português**

Build **não oficial** do emulador [shadPS4](https://github.com/shadps4-emu/shadPS4) com correções para **Shadow of the Colossus** (PS4, CUSA08809 EU, v1.01) no **Windows**. Não é distribuído pela equipe do shadPS4. **Nenhum arquivo do jogo está incluído**: você precisa do seu próprio dump.

### Baixar
Na aba **[Releases](../../releases)**, baixe `ShadPS4-SotC-0.2.7.rar` — versão **SotC 0.2.7**, mostrada no título da janela do emulador (o "Source code" listado lá é só o código).

### O que foi corrigido
- Sem travamento da GPU na abertura e sem quedas ao pular o vídeo ou carregar o save.
- Menu com texto, raios de luz do templo, sem blocos pretos e sem retângulos no chão.
- **Lago do pássaro:** sem clarões brancos em cruz nem pontos brancos piscando no horizonte quando o boss voa.
- Exposição automática correta em placas NVIDIA; menos "fantasma" do desfoque de movimento e sol menos estourado.
- 17–18 FPS no mundo aberto (era 13–15).

Detalhes técnicos de cada correção (causa e solução): [`documents/SotC-RELATORIO-REPORT.html`](documents/SotC-RELATORIO-REPORT.html) (baixe e abra no navegador; PT e EN).

### Lista completa de correções
Cada item tem causa, solução e commits no [relatório técnico](documents/SotC-RELATORIO-REPORT.html). ✅ = candidata ao shadPS4 oficial (genérica); 🎮 = específica do SotC (ligada só para `CUSA08809`).

**Núcleo e Windows**
| # | Correção | O que resolvia |
|---|---|---|
| 01 | ✅ Instruções SSE4a curtas realocadas | crash ~35 s em CPUs Intel (red zone apagada) |
| 02 | ✅ Tratador de falhas numa pilha alternativa | jogo fechava sozinho, sem log |
| 03 | ✅ Pilhas de thread/fibra nunca protegidas | quedas ao carregar o save |
| 04 | ✅ Proteções de página refeitas após SplitRegion | GPU com dados velhos, travamento (TDR) |
| 05 | 🎮 Proteção seletiva de red zone | crashes em 3 funções do jogo |

**Vulkan e sincronização**
| # | Correção | O que resolvia |
|---|---|---|
| 06 | ✅ Máscara de estágio para cada semáforo de espera | travamentos/deadlock |
| 07 | ✅ Device lost não congela o emulador | emulador preso para sempre |
| 08 | ✅ Cópia de profundidade entre formatos via buffer | queda ao pular o vídeo de abertura |
| 09 | ✅ Estágios não vazam entre pipelines do cache | crash nos primeiros quadros |
| 10 | ✅ Coerência de leituras indiretas/vértice e fences | GPU usando dados velhos |
| 11 | ✅ Memória de vídeo esgotada não aborta | queda em placas de 8 GB |
| 12 | ✅ Sem deadlock entre tratador de falhas e texturas | congelamento |
| 13 | ✅ Buffer de walkers SRT maior | crash no boot com cache grande |
| 27 | ✅ Páginas de pilha tratadas como páginas inteiras; readback da GPU nunca escreve nelas | quedas aleatórias após alguns minutos (heap do jogo corrompido) |
| 28 | ✅ Readback da GPU atravessando vários mapeamentos de memória (bug do upstream) | memória do jogo corrompida |
| 29 | ✅ Buffers em endereços não mapeados (lixo) ligados como nulos | queda na cena de abertura (memória de vídeo esgotada) |

**Recompilador de shaders**
| # | Correção | O que resolvia |
|---|---|---|
| 14 | ✅ ReadLane wave64 em GPUs de subgrupo 32 | reduções erradas na NVIDIA |
| 15 | 🎮 Ramos uniformes wave64 e barreiras LDS | faixas na iluminação |
| 16 | 🎮 Testes de profundidade antes do pixel shader | retângulos escuros no chão |
| 17 | ✅ ReadConst com índice dinâmico | blocos pretos (desfoque) |
| 18 | ✅ Outras (V_MAD_LEGACY, descritores lixo, limite de loops…) | asserts e travamentos |
| 24 | ✅ Comparações negadas com NaN | raios de luz do templo apagados |
| 25 | ✅ Instance ID relativo ao início do desenho | clarões no lago do pássaro |
| 26 | 🎮 Barreiras de subgrupo em reduções LDS | exposição automática errada |

**Desempenho** (13–15 → 17–18 FPS)
| # | Correção |
|---|---|
| 19 | 🎮 Envio periódico de comandos (o maior ganho) |
| 20 | ✅ Readbacks agrupados |
| 21 | ✅ Leituras limpas do walker SRT |
| 22 | ✅ Readback antecipado |

**Imagem**
| # | Correção |
|---|---|
| 23 | 🎮 Patch de shader: menos "fantasma" do motion blur e sol 20% menos estourado |

### Ferramentas e opções
Variáveis de ambiente (defina antes de abrir o `shadPS4.exe`, por exemplo num `.bat` com `set NOME=valor`). Os padrões já são os recomendados; mude só para testar.

| Variável | Padrão | Para que serve |
|---|---|---|
| `SOTC_FLUSH_EVERY` | 256 no SotC em NVIDIA, 0 em AMD/Intel | envio periódico de comandos; `0` desliga |
| `SOTC_RB_HOT` | 2 | readbacks agrupados (0 off, 1 conservador, 2 qualquer arena) |
| `SOTC_CLEAN_READS` | 1 | leituras limpas do walker SRT |
| `SOTC_RB_AHEAD` | 1 em NVIDIA, 0 em AMD/Intel | readback antecipado |
| `SOTC_EARLY_Z` | 1 no SotC | testes de profundidade antecipados |
| `SOTC_WAVE64_UNIFORM` | 1 no SotC | ramos uniformes wave64 e barreiras LDS |
| `SOTC_LDS_BARRIERS` | 1 no SotC | barreiras de subgrupo em grupos com várias waves |
| `SOTC_ARENA_RELEASE` | desligado | libera memória de arena no unmap (experimental) |
| `SHADPS4_LOOP_LIMIT` | 8192 no SotC | iterações máximas de loop por shader |
| `SHADPS4_REDZONE_PROTECT` | — | funções extras para proteção de red zone |
| `SHADPS4_IEEE_MINMAX` | desligado | volta ao min/max/clamp antigo |
| `SHADPS4_ABSOLUTE_INSTANCE_ID` | desligado | volta ao instance ID antigo |

**Ferramentas de diagnóstico** (usadas para achar os defeitos; desligadas por padrão):
- `SOTC_FRAME_LOG=N` — escreve a contagem de quadros e o tempo a cada N quadros (mede FPS).
- `SOTC_WRITE_RING=1` — registra as escritas do emulador na memória do jogo e, se o jogo cair, mostra no log quais caíram perto do crash (linhas `SOTCRING`).
- `SOTC_PROBE="0xENDEREÇO:N,…"` (+ `SOTC_PROBE_MS`) — sensor: grava em `user/log/sotc_probe.txt` os valores que a GPU calcula durante o jogo (ex.: exposição automática).
- `SOTC_DUMP=1` — com o RenderDoc desligado, **F12** salva em `user/log/dump_N/` as imagens reais de cada etapa da névoa volumétrica (achou o defeito do lago).
- `SOTC_OCCLUSION_STEP` — muda o contador falso de occlusion query (testes A/B).
- [`sotc/shader_patch/make_patch.py`](sotc/shader_patch) — regenera o patch de imagem (motion blur/sol) quando o recompilador muda.

### Modos (menu Trapaças / Modificações)
O pacote traz `user\patches\SotC-Modes` com **os dois modos já ligados**. Para desligar um: no launcher, clique com o botão direito no jogo → **Trapaças / Modificações** → aba **Modificações** → `SotC-Modes.xml`, desmarque e clique em **Salvar**. Fonte: [`sotc/patches/SotC-Modes`](sotc/patches/SotC-Modes).

| Modo | O que faz | Medido (lago do pássaro, RTX 2060 SUPER) |
|---|---|---|
| Texturas nítidas (16x AF) | filtro anisotrópico 16x em todas as texturas: chão, grama e rochas ficam nítidos de longe | sem custo de FPS (29,5 vs 28,5) |
| 60 FPS | tira o limite de 30 FPS do jogo (patch de illusion, GoldHEN) | média de 33 FPS, até ~46 |

Os dois podem ser combinados. Não ligue *Texturas nítidas* junto com o *Custom Debug Menu Config* do repositório shadPS4 (escrevem no mesmo lugar). Ainda não é possível: 1440p/2160p (o jogo só renderiza assim no modo PS4 Pro, que ainda fecha no início no shadPS4) e 21:9/19:9 (o jogo não tem ajuste de resolução nem de proporção).

### Como usar
1. Extraia o rar numa pasta com espaço (não em "Arquivos de Programas").
2. **Áudio — sem isto o jogo fica MUDO:** copie `libSceNgs2.sprx` e `libSceUlt.sprx` do firmware do **seu** PS4 para `user\sys_modules\` (firmware da Sony, não pode ser distribuído aqui). Com GoldHEN: ative o FTP nas configurações do GoldHEN, conecte pelo PC (ex.: FileZilla) no IP do PS4, porta `2121`, abra `/system/common/lib/` e copie os dois arquivos. Se você já usa shadPS4 com outros jogos, copie-os da sua pasta `sys_modules`.
3. Abra o `shadPS4QtLauncher.exe` da pasta: ele já vem configurado com este emulador (sem atualizações automáticas). Na primeira vez, escolha a pasta dos jogos e dê dois cliques no jogo. Também dá para arrastar o `eboot.bin` para `Iniciar-SotC.bat`.
4. A primeira abertura compila os shaders e demora mais.

Testado em Windows 11, Intel i9-13900K e NVIDIA RTX 2060 SUPER 8 GB. Não testado em placas AMD/Intel nem em Linux/macOS.

### Código-fonte
Este repositório é o próprio shadPS4 (base `259e815a`) com as correções, commit por commit; o README original do shadPS4 está em [`README.shadPS4.md`](README.shadPS4.md). Compilação: igual ao shadPS4 ([`documents/building-windows.md`](documents/building-windows.md)). As correções genéricas estão marcadas no relatório como candidatas ao shadPS4 oficial.

---

**License:** shadPS4 is GPL-2.0; this build and its source follow the same license. shadPS4QtLauncher (GPL-2.0), Qt 6 (LGPL-3.0) and FFmpeg (LGPL-2.1+) are included unmodified.
**Credits:** [shadPS4](https://github.com/shadps4-emu/shadPS4) · [shadPS4QtLauncher](https://github.com/shadps4-emu/shadps4-qtlauncher) · ideas and code ported from [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) by luizgustavs.
