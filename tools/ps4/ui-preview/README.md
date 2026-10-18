# Prévia da dock PSBOX

## Conceito de biblioteca

Abra `library-concept.html` para ver um conceito interativo da biblioteca no estilo visual da dock. É uma página HTML independente, com títulos demonstrativos e áreas neutras para capas; não baixa, copia nem embute artes. Busca, filtros, seleção por teclado e botões são demonstrações visuais e ainda não estão ligados ao catálogo ou à sessão do PSBOX.

Com o servidor local ativo, a página fica em `http://127.0.0.1:8765/tools/ps4/ui-preview/library-concept.html`. Para abrir pelo sistema de arquivos, também pode abrir o HTML diretamente no navegador.

A referência definitiva é `dock-reference.html`, cópia do HTML do usuário. O módulo compartilhado com o PS4 fica em `ps4-homebrew/stream_menu_ui.cpp/.h`; o HTML é referência visual, não um WebView embutido no console.

## Abrir no PC

```sh
./tools/ps4/ui-preview/open-native-preview.sh
```

O script compila uma biblioteca host temporária e abre uma janela PyQt. Não gera PKG nem compila o app PS4. A troca do arquivo da biblioteca é atômica, preservando janelas que ainda usam a versão anterior.

A janela chama o renderer C++ diretamente por `native_live.cpp`, em memória, com fontes e cache persistentes. Atualizações são agendadas a cada 16 ms durante transições ou mudanças; após estabilizar, não há redesenho contínuo. O temporizador não comprova 60 FPS efetivos. Não são lançados subprocessos nem gravados PNGs a cada quadro. A área de referência é 1920×1080, redimensionada sem deformar na janela.

## Controle no PC

- Q/E ou 1–4: percorrer os cartões na mesma ordem visual do PS4: Imagem → Volume → Configurações → Gamepad.
- Esquerda/direita com foco na dock: trocar ícone/painel.
- Cima ou Enter com foco na dock: entrar nas opções.
- Cima/baixo nas opções: escolher linha; esquerda/direita: alterar.
- Enter: selecionar; no Volume, silenciar/reativar.
- T: opções FSR (Triângulo no PS4).
- F5/F6/F7: simular vídeo normal, vídeo parado ou desconexão. Compacta mostra somente FPS, RTT em ms e bitrate RTP recebido; acrescenta uma mensagem nos dois últimos estados; Completa permite escolher Cartão/Linhas/Denso. As funções de desenho do Legado e dos alertas são compartilhadas com o PS4.
- Esc: voltar do FSR, retornar à dock, recolher o painel e, por fim, fechar a janela.

O preview liga `native_live.cpp`, `stream_menu_ui.cpp` e `stream_stats_ui.cpp` como uma biblioteca C++ e chama os mesmos métodos de desenho usados no PS4. A navegação por teclado traduz os cartões para os mesmos IDs estáveis e ordem visual usados pelo controle no console. O host usa Canvas CPU, não o backend Orbis/Vulkan/VideoOut, e seus valores de stream são demonstrativos; ele não lê o perfil salvo no PS4 nem coleta métricas reais. Alterações no código do renderer exigem reabrir a janela; uma janela aberta mantém a biblioteca que carregou na inicialização.

### OSD configurável de estatísticas

O painel novo e as seis páginas de configuração são desenhados pelo mesmo C++ usado no PS4 (`ps4-homebrew/stream_stats_ui.cpp` e `stream_menu_ui.cpp`). A prévia abre com o modo Completo e o OSD novo ligados para inspeção. No cartão Configurações, vá à opção `Configurar OSD` e confirme para abrir as opções. Use ↑/↓ para escolher o campo, ←/→ para alterar e `[`/`]` para mudar entre Layout, Espaçamento, Visual, Rede, Vídeo e Limites. Enter salva as opções do OSD; Esc descarta a edição e retorna. A configuração de perfis completa está ligada ao JSON do app PS4, não ao processo de prévia Linux.

O PS4 mantém a opção `Formato OSD: Cartão/Linhas/Denso`; Linhas e Denso usam os dois HTMLs alternativos fornecidos pelo usuário. O painel diagnóstico antigo permanece como fallback interno no PS4 se o renderer novo lançar uma exceção. Cartão começa selecionado em instalações sem preferência anterior; Linhas e Denso são alternativas na dock. A fonte da família continua Inter porque são as fontes empacotadas no app; a escolha Regular/Semibold/Bold muda o peso. Perda e jitter usam o último relatório RTCP, e “Perdidos/s” usa a variação do contador de quadros descartados. A prévia usa amostras ilustrativas; métricas do console só existem durante uma sessão real.

A UI mostra ícones vetoriais dos comandos do DualShock 4, desenhados pelo Canvas compartilhado, em vez dos nomes escritos dos botões. L1/R1 mantêm a identificação gravada nas próprias teclas desenhadas. O ícone Controles representa um DualShock 4 com touchpad e dois analógicos. Não são usados ícones Xbox nos comandos do controle; “Xbox Guide” é o nome da ação do serviço.

## Fidelidade e animações

Dock de 64 px, intervalo de 14 px, painel base de 380 px, preenchimento de 14 px e escala calculada pela mesma fórmula de `fit()` no HTML (altura de referência do maior conteúdo aproximada em 400 px). Linhas compartilham o contorno do grupo e os seletores têm trilho escuro com indicador deslizante. Não há o título/flutuante/rodapé adicionados pela primeira portabilidade.

- Indicadores e foco das linhas: 150 ms, `cubic-bezier(.215,.61,.355,1)`.
- Cores dos ícones: 220 ms.
- Painéis: 420 ms, `cubic-bezier(.22,1,.36,1)`, crescendo acima do ícone.
- Opacidade do painel: 220 ms ao abrir, 180 ms ao fechar.
- Conteúdo: entrada em 240 ms após atraso de 120 ms.

O estado de animação é da thread de desenho; mudanças interrompidas partem do valor interpolado atual. O mesmo código é ligado ao caminho PS4, incluindo invalidação do HUD enquanto há animação. A renderização no PC usa Canvas CPU; não verifica desempenho, proporção final do framebuffer, VideoOut ou Vulkan do PS4. Fonte Inter e caminhos geométricos dos ícones podem diferir do navegador. As opções de Controles do HTML ainda sem backend (sensibilidade, zona morta, vibração, tempo ajustável de OPTIONS) permanecem pendentes; a página mostra atalhos reais. Configurações preserva as opções de diagnóstico existentes.

## Validação

A versão animada foi compilada e aberta no Linux. Foram conferidos os quatro painéis, posições intermediárias do seletor, abertura/troca/fechamento, submenu FSR e retorno ao estado sem animação ativa. O usuário aprovou o visual da dock e os ícones PS4 na prévia do PC em 2026-10-17. Essa aprovação não valida desempenho nem execução no console. A build PS4 98 foi compilada e enviada após a aprovação do usuário; instalação/execução e desempenho no console continuam sem confirmação. O submenu FSR apresenta agora o alvo EASU calculado em pixels (por exemplo, 1920×1080 ou 3840×2160), em vez do fator 1×/2×.

Para PNG estático, quando necessário:

```sh
./tools/ps4/ui-preview/build-native-preview.sh settings fsr=1 tier=1080 source=1080
```

Também aceita `controls`, `volume`, `configuration` e ajustes demonstrativos `fsr=1`, `tier=1080`, `source=1080` (`source=720` simula fonte 720p). A linha “Alvo EASU” usa a resolução de saída e mostra 3840×2160 apenas se tier pedido, fonte e saída permitirem o caminho 2×. O PNG estático não demonstra as transições. `index.html`, `style.css` e `app.js` são o mockup anterior de três cartões.

A tipografia do formato Cartão segue o HTML original: base 13 px e escala 120%, unidades a 85%, rótulos dos cartões a 68% e títulos de grupo a 60%. O renderer converte em para a altura nativa da fonte apenas no OSD; a dock conserva sua tipografia. Cartão põe FPS no cabeçalho e métricas agrupadas horizontalmente, Linhas é vertical e Denso distribui as métricas em duas linhas. Os comandos de abrir/salvar/voltar/paginar usam os ícones PS4 existentes.

Ao trocar o formato, a aparência recebe os valores do HTML correspondente. Linhas/Denso: 16 px, escala 100%, linha 120%, margens 12 px, padding 6, fundo 0%, contorno 1 px. A seleção de métricas e os limites de alerta permanecem; ajustes visuais feitos depois podem ser salvos no perfil ativo. Trocar novamente de formato reaplica o preset visual.

### Posição e bitrate do HUD

O OSD inicia no canto superior esquerdo. Em Configurar OSD → Layout → Posição, escolha superior esquerdo/direito ou inferior esquerdo/direito. A posição vale para Compacta e Completa e é persistida. O bitrate mostra somente o valor recebido em Mbps; o teto REMB permanece nos diagnósticos. Perfis antigos sem position_version migram uma vez para superior esquerdo. Os três HTMLs de referência ficam em `references/`; Linhas/Denso têm aparência própria, com posição padrão superior esquerda por pedido posterior do usuário.
