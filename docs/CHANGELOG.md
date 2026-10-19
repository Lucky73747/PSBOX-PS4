# Changelog

## 0.1.1 — Release candidate (Build 18)

- Added a persistent artwork cache and expanded the library with **My Games** and **Recent** views.
- Added library localization with English as the default, plus Brazilian Portuguese, Spanish, French, German and Italian.
- Improved controller navigation in title search and added Circle to cancel a pending game launch request.
- Reduced the full PKG from 38,862,848 bytes (build 17) to 34,340,864 bytes (build 18), an 11.6% reduction. The smaller package was tested on a PS4; broader release review is still pending.
- Package: `PSBOX-0.1.1-beta-b18.pkg`; title ID `PSBX00001`; installer version `00.03`.
- This is a release candidate. It has not been published as a GitHub release yet. Binary distribution review for the exact external SDK/toolchain inputs remains open; see [LICENSE-STATUS.md](LICENSE-STATUS.md).

## Português — 0.1.1 candidata (Build 18)

- Adiciona cache persistente de artes e as abas **Meus jogos** e **Recentes**.
- Adiciona idiomas à biblioteca: inglês padrão, português brasileiro, espanhol, francês, alemão e italiano.
- Melhora a navegação por controle na busca de títulos e permite cancelar um pedido de início de jogo com Círculo.
- Reduz o PKG completo de 38.862.848 bytes (build 17) para 34.340.864 bytes (build 18), uma redução de 11,6%. O pacote menor foi testado em um PS4; a revisão mais ampla da release ainda está pendente.
- Pacote: `PSBOX-0.1.1-beta-b18.pkg`; identificador `PSBX00001`; versão do instalador `00.03`.
- Esta é uma candidata à release e ainda não foi publicada no GitHub. A revisão de distribuição binária dos insumos externos exatos do SDK/toolchain continua pendente; consulte [LICENSE-STATUS.md](LICENSE-STATUS.md).

## 0.1.0 Beta — Build 1

### Initial beta

- Account sign-in and game catalog browsing.
- PS4 hardware H.264 decoding with Vulkan rendering.
- Bilinear and FidelityFX FSR1 scaling, with configurable sharpening.
- Opus audio, in-game dock, controller navigation and configurable statistics.
- New PS4 application identity: **PSBOX**, title ID `PSBX00001`, installer version `00.01`.
- Build numbering restarts at **1** for this beta series. Earlier internal build 107 is not beta build 1.
- Package naming: `PSBOX-0.1.0-beta-b1.pkg`.
- Capture helper defaults to the new title ID and retains recognition of old crash directories.

### Known issues and validation status

- Build 1 was compiled and packaged on GitHub-hosted Ubuntu in [run 39092806118](https://github.com/Lucky73747/PSBOX-PS4/actions/runs/39092806118). The artifact records the exact compiled commit. PS4 runtime testing remains pending.
- Earlier keyboard fixes address UTF-16 input, controller suppression while the IME is open, and dialog termination. Actual text delivery to games still requires console validation; local data-channel acceptance does not confirm game receipt.
- The earlier session-close fix moves teardown to a worker and uses the dock exit button. Its effect on reported freezes still requires console validation.
- Audio synchronization and recovery remain limited. Video stutter, freeze recovery and UI responsiveness require testing under real network and console conditions.
- Requested resolution and REMB bitrate are not guaranteed delivery settings; the service may choose another resolution or bitrate.
- External SDK/component licensing and corresponding-source requirements remain listed in [LICENSE-STATUS.md](LICENSE-STATUS.md).

### Installation compatibility

`PSBX00001` installs as a separate application from the old `XCHM00001`; it is not an in-place update. Persistent data paths have not been migrated by this identity change. Do not assume a fresh installation clears existing account data or profiles.

### Bug reports

Include version/build, PS4 model, reproduction steps and redacted app log/Klog from the same run. Report through the [development lab](https://discord.gg/ap5GvUQsxs). Never upload account files, tokens, login codes or raw session dumps.

## Português

A Beta 0.1 começa na **build 1**, com nome PSBOX e identificador `PSBX00001`. Instala separadamente do app antigo. O PKG esperado será `PSBOX-0.1.0-beta-b1.pkg`, compilado no GitHub Actions e acompanhado de SHA-256. Ainda não foi validado no console.

Pontos para testar: teclado e entrega do texto ao jogo, saída da sessão sem congelamento, sincronização de áudio, recuperação de vídeo e fluidez da UI. Esses itens são pendências de validação, não correções comprovadas nesta build. A resolução e o bitrate efetivamente recebidos dependem também do serviço. Consulte as pendências de licenciamento antes de distribuir binários.
