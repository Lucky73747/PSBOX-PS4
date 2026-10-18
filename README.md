# PSBOX for PS4

PSBOX is an independent homebrew client for PlayStation 4. It is intended to let users access and play games available to their own authorized account. PSBOX does not provide games, subscriptions, accounts, or access rights. Use only accounts and content you are authorized to use.

PSBOX is not an Xbox or PlayStation product and is not affiliated with, endorsed by, or sponsored by Microsoft, Xbox, Sony, or PlayStation. The current implementation connects to Microsoft's game-streaming service; this describes technical compatibility, not ownership or endorsement. All trademarks belong to their respective owners.

This source identifies app version **0.1.0**, build **1**. The new PS4 title ID is `PSBX00001`, with installer version `00.01`; it installs separately from the old `XCHM00001` app. Package name: `PSBOX-0.1.0-beta-b1.pkg`. The current source snapshot has not yet been rebuilt as the 0.1.0 package or validated in a console session. No PKG is included here.

## Beta 0.1

The beta series starts at **build 1**. See the [initial changelog and known issues](docs/CHANGELOG.md). The older internal build 107 PKG is not this beta build. No matching build 1 package has been compiled or attached yet.

## Features and limitations

- Account sign-in and game catalog browsing.
- PS4 hardware H.264 decoding and Vulkan-based video rendering.
- Bilinear and FidelityFX FSR1 video scaling options.
- Opus audio output, controller navigation, stream controls, and configurable statistics.

Available games, resolution, bitrate, latency, and stability depend on the user's account, service availability, title, network, and console. Hardware behavior and recent changes need testing on the target PS4. Audio synchronization and recovery features remain limited; see the source and issue tracker for current status.

## Build

Start with [BUILDING.md](BUILDING.md) and [BUILD-PREREQUISITES.md](BUILD-PREREQUISITES.md). Compatible PS4 SDK, Mesa/RADV build products, porting kit, and SDL3 PS4 inputs must be obtained separately from authorized sources. These external inputs, generated packages, and console data are not included.

The build command is:

```sh
tools/ps4/build.sh --check-inputs
tools/ps4/build.sh
```

The first command checks for required inputs; the second builds the app and package. A successful build confirms compilation and packaging only, not runtime behavior. The source-check workflow validates the selected tree. A separate manual GitHub-hosted compilation workflow produces a PKG artifact using private external inputs; see [BUILDING.md](BUILDING.md).

## Source checks

```sh
python3 tools/release/check-public-tree.py
python3 tools/release/export-public-source.py --output ../PSBOX-PS4-source
```

The exporter creates a source-only snapshot without Git history or generated build artifacts. Review its contents and the repository's applicable license obligations before publishing. The source check does not certify runtime behavior or binary redistribution rights.

## Community and reports

The [PSBOX Discord lab](https://discord.gg/ap5GvUQsxs) is a shared workshop for the developer's projects; it is not exclusive to PSBOX. Bug reports should include the app log and Klog for the same run, plus the app version, PS4 model, and reproduction steps. The included [PS4 Manager](tools/ps4/ps4-pc-manager/) can capture logs. Review and redact account identifiers, IP addresses, tokens, login codes, SDP/ICE data, and other personal information before sharing. Do not post raw account files or session data.

PSBOX is developed primarily by one person, with AI assistance. Development may be slow, and bugs are expected. Contributions and careful reports are welcome; no one needs to donate to participate.

Contact: [stillness.dev@gmail.com](mailto:stillness.dev@gmail.com).

## Credits and license

PSBOX includes and adapts work from RafaelNGP's [PS5 homebrew client](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5); upstream copyright and license notices are retained. Protocol and platform references are listed in [DEPENDENCIES.md](DEPENDENCIES.md) and [docs/SOURCE-PROVENANCE.md](docs/SOURCE-PROVENANCE.md).

PSBOX source is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party components retain their own terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses/README.md](licenses/README.md). Source availability does not by itself grant rights to redistribute third-party SDK inputs or a compiled package.

### Português

PSBOX é um cliente homebrew independente para PlayStation 4. A proposta é permitir que cada pessoa acesse e jogue títulos disponíveis na própria conta autorizada. O PSBOX não fornece jogos, assinaturas, contas ou direitos de acesso. Use apenas contas e conteúdos que você tem autorização para utilizar.

O PSBOX não é um produto da Xbox, Microsoft, PlayStation ou Sony e não é afiliado, endossado ou patrocinado por essas empresas. A implementação atual se conecta ao serviço de jogos em nuvem da Microsoft; isso descreve compatibilidade técnica, não propriedade ou endosso. As marcas pertencem aos respectivos titulares.

Consulte o [changelog inicial e os problemas conhecidos](docs/CHANGELOG.md). A antiga build interna 107 não corresponde à nova build 1.

Este código identifica a versão **0.1.0**, build **1**. O novo identificador PS4 é `PSBX00001`, com instalador `00.01`; ele instala separadamente do antigo `XCHM00001`. Nome do pacote: `PSBOX-0.1.0-beta-b1.pkg`. Esta cópia ainda não foi recompilada como pacote 0.1.0 nem validada em uma sessão no console. Nenhum PKG está incluído.

O app oferece login e catálogo, decodificação H.264 por hardware no PS4, renderização Vulkan, escalonamento bilinear e FidelityFX FSR1, saída de áudio Opus, navegação por controle e estatísticas configuráveis. Jogos disponíveis, resolução, bitrate, latência e estabilidade dependem da conta, serviço, título, rede e console. Uma compilação concluída confirma somente compilação e empacotamento, não o funcionamento no PS4.

Consulte [BUILDING.md](BUILDING.md) e [BUILD-PREREQUISITES.md](BUILD-PREREQUISITES.md) para compilar. Os insumos externos do SDK e do SDL3 para PS4 precisam ser obtidos de fontes autorizadas e não estão incluídos. O código do PSBOX está sob GPL-3.0-or-later; componentes de terceiros mantêm suas próprias licenças.

O [laboratório do Discord](https://discord.gg/ap5GvUQsxs) reúne os projetos do desenvolvedor, não apenas o PSBOX. Relatos de bugs devem incluir o log do app e o Klog da mesma execução, além da versão, modelo do PS4 e passos para reproduzir. O [PS4 Manager](tools/ps4/ps4-pc-manager/) ajuda a capturar os logs. Antes de compartilhar, revise e remova identificadores de conta, endereços IP, tokens, códigos de login, dados SDP/ICE e outras informações pessoais. Não publique arquivos de conta nem dados brutos de sessão.

O PSBOX é desenvolvido principalmente por uma pessoa, com auxílio de IA. O desenvolvimento pode ser lento e haverá bugs. Contribuições e relatos cuidadosos são bem-vindos; não é necessário doar para participar. Contato: [stillness.dev@gmail.com](mailto:stillness.dev@gmail.com).
