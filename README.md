# ShadPS4 · Shadow of the Colossus — build customizado (97% corrigido)

**🇧🇷 Português** · [English below](#english)

> [!IMPORTANT]
> **Para jogar, baixe o zip em [Releases](../../releases)** (`SotC-shadPS4-fixes-…zip`). O botão verde **Code → Download ZIP** traz só o código-fonte, sem o emulador pronto. Não use o shadPS4 que o Launcher baixa sozinho (oficial/Nightly): ele não tem estas correções. **Som:** o jogo precisa de 2 arquivos do firmware do seu PS4 (veja "Como usar", passo 2); sem eles fica mudo.
>
> **To play, download the zip from [Releases](../../releases)** (`SotC-shadPS4-fixes-…zip`). The green **Code → Download ZIP** button only gets the source code, not the built emulator. Do not use the shadPS4 the Launcher downloads by itself (official/Nightly): it lacks these fixes. **Sound:** the game needs 2 files from your own PS4 firmware (see "How to use", step 2); without them it is silent.

Build **não oficial** do emulador [shadPS4](https://github.com/shadps4-emu/shadPS4) com correções para **Shadow of the Colossus** (PS4, CUSA08809 EU, v1.01) no **Windows**. Não é distribuído pela equipe do shadPS4. **Nenhum arquivo do jogo está incluído**: você precisa do seu próprio dump.

### Baixar
Na aba **[Releases](../../releases)**, baixe `SotC-shadPS4-fixes-2026-10-03.zip` (o "Source code" listado lá é só o código).

### O que foi corrigido
- Sem travamento da GPU na abertura e sem quedas ao pular o vídeo ou carregar o save.
- Menu com texto, raios de luz do templo, sem blocos pretos e sem retângulos no chão.
- **Lago do pássaro:** sem clarões brancos em cruz nem pontos brancos piscando no horizonte quando o boss voa.
- Exposição automática correta em placas NVIDIA; menos "fantasma" do desfoque de movimento e sol menos estourado.
- 17–18 FPS no mundo aberto (era 13–15).

Detalhes técnicos de cada correção (causa e solução): [`documents/SotC-RELATORIO-REPORT.html`](documents/SotC-RELATORIO-REPORT.html) (baixe e abra no navegador; PT e EN).

### Como usar
1. Extraia o zip numa pasta com espaço (não em "Arquivos de Programas").
2. **Áudio — sem isto o jogo fica MUDO:** copie `libSceNgs2.sprx` e `libSceUlt.sprx` do firmware do **seu** PS4 para `user\sys_modules\` (firmware da Sony, não pode ser distribuído aqui). Com GoldHEN: ative o FTP nas configurações do GoldHEN, conecte pelo PC (ex.: FileZilla) no IP do PS4, porta `2121`, abra `/system/common/lib/` e copie os dois arquivos. Se você já usa shadPS4 com outros jogos, copie-os da sua pasta `sys_modules`.
3. Abra o `shadPS4QtLauncher.exe` da pasta: ele já vem configurado com este emulador (sem atualizações automáticas). Na primeira vez, escolha a pasta dos jogos e dê dois cliques no jogo. Também dá para arrastar o `eboot.bin` para `Iniciar-SotC.bat`.
4. A primeira abertura compila os shaders e demora mais.

Testado em Windows 11, Intel i9-13900K e NVIDIA RTX 2060 SUPER 8 GB. Não testado em placas AMD/Intel nem em Linux/macOS.

### Código-fonte
Este repositório é o próprio shadPS4 (base `259e815a`) com as correções, commit por commit; o README original do shadPS4 está em [`README.shadPS4.md`](README.shadPS4.md). Compilação: igual ao shadPS4 ([`documents/building-windows.md`](documents/building-windows.md)). As correções genéricas estão marcadas no relatório como candidatas ao shadPS4 oficial.

---

<a id="english"></a>
**🇺🇸 English**

**Unofficial** build of the [shadPS4](https://github.com/shadps4-emu/shadPS4) emulator with fixes for **Shadow of the Colossus** (PS4, CUSA08809 EU, v1.01) on **Windows**. It is not distributed by the shadPS4 team. **No game files are included**: you need your own dump.

### Download
From the **[Releases](../../releases)** tab, get `SotC-shadPS4-fixes-2026-10-03.zip` (the "Source code" entries there are only the code).

### What is fixed
- No GPU hang in the intro and no crashes when skipping the intro video or loading the save.
- Menu text, the temple light shafts, no black blocks, no rectangles on the ground.
- **Bird colossus lake:** no cross-shaped white flashes or flickering white dots on the horizon while the boss flies.
- Correct auto-exposure on NVIDIA GPUs; less motion-blur ghosting and a less blown-out sun.
- 17–18 fps in the open world (was 13–15).

Technical details of every fix (cause and solution): [`documents/SotC-RELATORIO-REPORT.html`](documents/SotC-RELATORIO-REPORT.html) (download and open in a browser; PT and EN).

### How to use
1. Extract the zip somewhere with free space (not "Program Files").
2. **Audio — without this the game is SILENT:** copy `libSceNgs2.sprx` and `libSceUlt.sprx` from **your own** PS4 firmware into `user\sys_modules\` (Sony firmware, it cannot be distributed here). With GoldHEN: enable FTP in the GoldHEN settings, connect from the PC (e.g. FileZilla) to the PS4's IP, port `2121`, open `/system/common/lib/` and copy both files. If you already use shadPS4 with other games, copy them from your `sys_modules` folder.
3. Open the folder's `shadPS4QtLauncher.exe`: it comes preconfigured with this emulator (no automatic updates). The first time, pick your games folder, then double-click the game. You can also drag `eboot.bin` onto `Iniciar-SotC.bat`.
4. The first start compiles the shaders and takes longer.

Tested on Windows 11, Intel i9-13900K and NVIDIA RTX 2060 SUPER 8 GB. Not tested on AMD/Intel GPUs, Linux or macOS.

### Source code
This repository is shadPS4 itself (base `259e815a`) with the fixes, commit by commit; the original shadPS4 README is in [`README.shadPS4.md`](README.shadPS4.md). Building: same as shadPS4 ([`documents/building-windows.md`](documents/building-windows.md)). Generic fixes are marked in the report as upstream candidates.

---

**License:** shadPS4 is GPL-2.0; this build and its source follow the same license. shadPS4QtLauncher (GPL-2.0), Qt 6 (LGPL-3.0) and FFmpeg (LGPL-2.1+) are included unmodified.
**Credits:** [shadPS4](https://github.com/shadps4-emu/shadPS4) · [shadPS4QtLauncher](https://github.com/shadps4-emu/shadps4-qtlauncher) · ideas and code ported from [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) by luizgustavs.
