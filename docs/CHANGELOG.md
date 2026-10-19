# Changelog

## 0.1.1 Beta — Build 26

- Mapped **L3 + R3** to the Xbox **View/Back** input. The touchpad remains mapped to Guide; this chord no longer triggers the manual video reset.
- Added a controller guide in English and Brazilian Portuguese, and documented the shortcut in the in-game controls card.
- Persisted **Recent** games and the account-scoped **My Games** snapshot across launches; the library displays cached games while refreshing.
- Added persistent controller light-bar settings and a localized, optional in-game dock tutorial.
- Improved library loading feedback, captions, console-search alignment and menu transitions.
- Ensured native 1:1 output bypasses both EASU and RCAS.
- Package: `PSBOX-0.1.1-beta-b26.pkg`; title ID `PSBX00001`; installer version `00.04`.
- Package SHA-256: `1857d8afcc8a0050d8d4ba7d73ce10ebb01af34bdfe08c17882c7392e4daedb0`.
- Built locally and uploaded to the owner's PS4 for testing. This build has not yet been confirmed running on-console.

## Português — 0.1.1 Beta — Build 26

- **L3 + R3** agora envia o botão **View/Voltar** do controle Xbox. O touchpad continua como Guide; essa combinação não aciona mais o reset manual do vídeo.
- Adicionado um guia de controles em inglês e português brasileiro, além da indicação do atalho no cartão de controles durante o jogo.
- **Recentes** e o snapshot de **Meus jogos**, separado por conta, persistem entre inicializações; a biblioteca mostra os dados em cache enquanto atualiza.
- Adicionadas configurações persistentes da barra de luz e um tutorial localizado e opcional para abrir a dock durante o jogo.
- Melhorados o feedback de carregamento da biblioteca, as legendas, o alinhamento da busca por consoles e as transições dos menus.
- Garantido que a escala nativa 1:1 não execute EASU nem RCAS.
- Pacote: `PSBOX-0.1.1-beta-b26.pkg`; identificador `PSBX00001`; versão do instalador `00.04`.
- SHA-256 do pacote: `1857d8afcc8a0050d8d4ba7d73ce10ebb01af34bdfe08c17882c7392e4daedb0`.
- Compilado localmente e enviado ao PS4 do responsável para testes. Ainda não há confirmação de execução desta build no console.

## 0.1.1 Beta — Build 18 (previous beta release)

- Added a persistent artwork cache and expanded the library with **My Games** and **Recent** views.
- Added library localization with English as the default, plus Brazilian Portuguese, Spanish, French, German and Italian.
- Improved controller navigation in title search and added Circle to cancel a pending game launch request.
- Reduced the full PKG from 38,862,848 bytes (build 17) to 35,782,656 bytes (build 18), a 7.9% reduction. The owner reported that build 18 worked on a PS4.
- Package: `PSBOX-0.1.1-beta-b18.pkg`; title ID `PSBX00001`; installer version `00.03`.
- Published as the `v0.1.1-beta.1` GitHub prerelease with corresponding-source materials.

## Português — 0.1.1 Beta — Build 18 (versão beta anterior)

- Adiciona cache persistente de artes e as abas **Meus jogos** e **Recentes**.
- Adiciona idiomas à biblioteca: inglês padrão, português brasileiro, espanhol, francês, alemão e italiano.
- Melhora a navegação por controle na busca de títulos e permite cancelar um pedido de início de jogo com Círculo.
- Reduz o PKG completo de 38.862.848 bytes (build 17) para 35.782.656 bytes (build 18), uma redução de 7,9%. O responsável pelo projeto informou que a build 18 funcionou em um PS4.
- Pacote: `PSBOX-0.1.1-beta-b18.pkg`; identificador `PSBX00001`; versão do instalador `00.03`.
- Publicada como pré-release `v0.1.1-beta.1` no GitHub, acompanhada dos materiais de código-fonte correspondente.

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
