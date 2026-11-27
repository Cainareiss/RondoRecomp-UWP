# RondoRecomp UWP para Xbox Series S|X

Base experimental iniciada a partir de [gminustwo/RondoRecomp](https://github.com/gminustwo/RondoRecomp),
commit `8114a2cb498310043addaf73af1401181dbadac5`. O destino é **Xbox Dev Mode,
UWP x64**, com um frontend próprio para controle. Este checkout contém o código
do frontend, mas ainda não tem um executável ou MSIX validado. A execução no
Windows e no Xbox ainda não foi testada; não há desempenho de console medido.
Nenhum disco, ROM, BIOS, música ou código gerado do jogo acompanha o repositório.

## O que esta base implementa

- O núcleo C do upstream fica atrás de `ports/uwp/CoreSession.h`, sem SDL no
  frontend UWP: CPU, vídeo, áudio, leitura CHD/CUE, BRAM e um slot de save state.
- `App.cpp` usa CoreWindow/C++/WinRT, Windows.Gaming.Input e pastas locais do
  aplicativo. O relógio mantém a frequência original, aproximadamente 59,826 Hz,
  independentemente de a tela estar em 60 ou 120 Hz, com recuperação de atraso limitada.
- `AV.cpp` apresenta o framebuffer em Direct3D 11, com proporção 4:3, e envia
  PCM estéreo 44,1 kHz ao XAudio2 em uma fila limitada. Os shaders são compilados
  no computador de desenvolvimento, nunca no Xbox.
- O tile, a splash screen e os ícones usam a arte enviada pelo usuário. Duas
  molduras PNG selecionáveis, com canal alfa, são compostas sobre o jogo; seus
  centros transparentes deixam a imagem visível e o frontend ajusta a abertura
  para preservar a proporção 4:3.
- Vibração nativa do controle Xbox está ligada por padrão: pulso curto e leve ao
  pular, mais forte ao atacar. O feedback é enviado apenas para ações do gamepad;
  teclado não vibra. A vibração é zerada ao pausar, suspender ou fechar.
- O botão B é consumido como evento de retorno do sistema UWP, evitando que o
  Xbox feche o aplicativo. Na tela de pausa, continua carregando o slot 1.
- Pausa, perda de foco, desconexão do controle, suspensão, BRAM periódica e
  consulta do limite real de memória têm tratamento inicial.
- O projeto MSBuild produz o APPX intermediário; o script gera um `.msix` x64
  validado a partir desse payload. `scripts/make_assets.py` verifica tamanhos e
  canal alfa, sem sobrescrever as artes do usuário.

Menus, filtros, rewind longo, RetroAchievements, atualizador e Setup desktop do
upstream ainda não foram migrados. Importar pelo seletor do sistema, persistência,
retomada e recuperação do dispositivo gráfico precisam de teste no console.

## Limites do Xbox considerados

A [documentação Microsoft de recursos UWP no Xbox](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
informa 1 GB para aplicativos e 5 GB para jogos em primeiro plano. A classificação
e o ambiente de execução importam: esta base não presume automaticamente os 5 GB.
Ela consulta `MemoryManager.AppMemoryUsageLimit()`, mostra uso/limite e pausa
quando o uso ultrapassa 90%. A fila de áudio e a recuperação de atraso são
limitadas; ainda é necessário medir o consumo completo no Series S sem depurador.

Esse mesmo documento exige x64 e informa DirectX 11/12 com feature level 11.0
nos Series S|X. A implementação inicial usa D3D11 com shaders `vs_4_0/ps_4_0`.
O AppContainer não oferece acesso geral ao disco do computador: o jogo é copiado
para o armazenamento do aplicativo antes de o núcleo abrir seus arquivos.
Não há JIT, executável filho ou compilador embarcado. Código recompilado é
gerado e compilado antes do empacotamento. O hardware completo do Series não é
o orçamento disponível a uma aplicação UWP.

## Compilar no Windows

Requisitos: Visual Studio 2022, MSVC v143, **Universal Windows Platform tools**,
**C++ (v143) Universal Windows Platform tools**, Windows SDK **10.0.22621.0** e
CMake, caso seja necessário gerar código do disco. Os IDs dos componentes estão
na [lista oficial do Visual Studio](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-community?view=vs-2022).
O pacote aceita Windows com versão mínima **10.0.19041.0**.

No PowerShell, a partir da raiz do repositório:

```powershell
# Base funcional do frontend, usando o interpretador do upstream.
./ports/uwp/scripts/Build-Uwp.ps1 -Configuration Release

# Recompilar seu disco em C no computador e incluir os trechos nativos no UWP.
# O destino de geracao precisa estar vazio; a imagem e as faixas nao sao empacotadas.
./ports/uwp/scripts/Build-Uwp.ps1 -DiscPath 'D:\MeuJogo\game.chd'

# Ou reutilizar um diretorio criado anteriormente por rondo_gen.
./ports/uwp/scripts/Build-Uwp.ps1 -GeneratedDir 'D:\Rondo\work\uwp-generated'
```

O MSIX final vai para `build/uwp/MSIX/`, com dependências UWP x64 em `Dependencies/`.
Os APPX intermediários ficam em `build/uwp/AppPackages/`. O empacotamento usa
MakeAppx com validação habilitada. Não misture dependências de desktop com as UWP x64.
`RondoGeneratedDir` seleciona `table.c` e os outros `.c` gerados e exclui
`recomp_none.c`; sem essa propriedade, a base usa o interpretador. O mesmo disco
usado na geração deve ser fornecido ao aplicativo. Sem os trechos gerados,
ainda não se validou um build de recompilação estática neste port.

O projeto usa C11 para o núcleo e C++17 com `/await:strict` para os arquivos C++
do frontend. Não usa C++/CX, `/ZW`, WinUI 3 ou desktop bridge. O nome
`RondoRecomp.App` no manifesto acompanha o modelo Core App; a entrada nativa
`wWinMain` passa uma implementação de `IFrameworkViewSource` a
`CoreApplication::Run`, conforme o [tutorial Microsoft](https://learn.microsoft.com/en-us/windows/uwp/gaming/tutorial--setting-up-the-games-infrastructure).

O workflow `.github/workflows/uwp.yml` prepara um runner Windows 2022 para
compilar, empacotar e assinar somente a base com interpretador. O workflow ainda
não foi executado neste repositório.

## Compilação cruzada no Linux

O script de compilação cruzada usa Clang/LLVM, Windows SDK 10.0.22621 e CRT Store
obtidos dos pacotes Microsoft. O frontend usa C++20 para as coroutines do Clang;
o núcleo continua C11. O projeto Visual Studio mantém C++17 com `/await:strict`.
Os mesmos fontes de App, áudio/vídeo e núcleo são usados.

O sysroot já está preparado nesta máquina. Para recriá-lo a partir dos downloads
oficiais em outro checkout Linux, o script requer aceitar a licença Microsoft:

```sh
dev python3 ports/uwp/scripts/prepare-linux-toolchain.py --accept-license
```

Para recompilar o executável e PDB:

```sh
dev python3 ports/uwp/scripts/build-linux.py --sysroot build/cross-toolchain/sysroot22621 --output build/uwp-linux
# Para incluir C gerado anteriormente a partir do seu disco, acrescente:
# --generated-dir work/uwp-generated
```

No ambiente de desenvolvimento atual, a tentativa de cross-build não concluiu:
o link falhou por falta de `OneCore.lib`. Também não estão disponíveis localmente
FXC, o pacote de dependência Microsoft.VCLibs UWP x64 e `osslsigncode`; portanto
nenhum MSIX foi produzido ou validado neste checkout. A seção abaixo descreve a
rota prevista, não uma entrega já testada. Os recursos usam caminhos literais,
sem dependência de `Resources.pri`, e o pacote declara Microsoft.VCLibs.140.00 UWP x64.

A assinatura local usa um certificado de desenvolvimento com o Publisher do
manifesto. Somente o `.cer` público acompanha a entrega; a chave privada fica no
diretório de build local e não faz parte do pacote ou do código distribuído.
Os logs de empacotamento e verificação ficam junto aos resultados da compilação.
Essa validação de formato e assinatura não substitui instalar e executar no Xbox.

Para repetir o empacotamento e a assinatura no ambiente já preparado:

```sh
dev python3 ports/uwp/scripts/package-linux.py
```

Quando todos os requisitos estiverem instalados, o resultado esperado fica em
`build/uwp/MSIX/`: MSIX assinado, certificado público,
Microsoft.VCLibs UWP x64, hashes, relatório de validação e instruções de instalação.
O `package-report.json` registra a verificação de schema/assinatura e o hash do
executável incluído. O manifesto atualmente declara a versão `0.2.0.0`; o modo
`interpreter-bootstrap` ainda não inclui trechos recompilados do disco do jogador.

### Assinatura local opcional

Para um pacote de desenvolvimento assinado:

```powershell
./ports/uwp/scripts/Sign-Uwp.ps1 -PackagePath 'D:\Rondo\build\uwp\MSIX\RondoRecomp-UWP_0.2.0.0_x64_interpreter-bootstrap.msix'
```

O script cria um certificado de desenvolvimento em `CurrentUser\My`, mantém a
chave privada no armazenamento de certificados e exporta somente o `.cer`
público ao lado do `.msix`. Para reutilizar o certificado, informe
`-CertificateThumbprint`. Não há certificado, chave, senha ou credencial no
repositório. O uso local padrão não instala o pacote nem altera a confiança de
certificados. O workflow usa `-VerifyWithTemporaryTrust` somente no runner
descartável e remove essa confiança temporária ao concluir a verificação.

## Fornecer o disco e usar o controle

Depois de instalar no Xbox pelo Device Portal, abra uma vez para criar as pastas.
No explorador de arquivos do aplicativo no Portal, copie para **LocalState/game**:

- Um único `.chd`; ou
- Um `.cue` com todas as faixas referenciadas, preservando nomes e caminhos relativos.

O `.cue` sozinho não contém o disco. Nesta etapa o aplicativo procura imagens na
raiz de `game`, escolhe a imagem importada mais recentemente ou a primeira por
nome e não apresenta uma biblioteca de jogos. Pressione A na tela inicial para
procurar novamente. X abre o seletor para copiar um CHD; se o seletor estiver
indisponível no Xbox, use o Portal. Arquivos externos não são abertos diretamente
pelo núcleo.

Cada importação cria uma cópia nova do CHD para preservar o disco aberto até a
cópia terminar. Isso consome espaço adicional: remova cópias antigas de
`LocalState/game` pelo Portal quando elas não estiverem em uso.

| Ação | Controle Xbox | Teclado de desenvolvimento |
| --- | --- | --- |
| Mover | Direcional / analógico esquerdo | Setas |
| Pular / botão I | A | Z |
| Atacar / botão II | X | X |
| Run | Menu | Enter |
| Select | LB | Shift |
| Pausar frontend | View | Esc |
| Continuar, na pausa | A | Enter |
| Salvar slot 1, na pausa | Y | F5 |
| Carregar slot 1, na pausa | B | F8 |
| Importar CHD, na tela inicial/pausa | X | I |
| Alternar moldura visual, na pausa | RB | O |

Há duas molduras selecionáveis. A escolha fica salva no armazenamento local e
é restaurada na próxima inicialização. A segunda usa a arte vermelha enviada
para as laterais; a área transparente central preserva a imagem do jogo.

Durante o jogo, cada novo toque de X (ataque) produz um pulso de rumble mais
forte; A (pulo) produz um pulso leve. B não é um comando de saída e não fecha o
aplicativo; na pausa ele carrega o slot 1.

BRAM e save states ficam em **LocalState/saves**, com nomes por identidade do
disco. Faça backup dessa pasta antes de remover/reinstalar o aplicativo.
As gravações passam por arquivo temporário antes da substituição; no Windows,
o arquivo substituído também fica em `.bak`. Save states dependem da estrutura
do núcleo e não têm compatibilidade garantida entre versões ou Linux e Windows.

## Verificação portátil e próxima validação

O teste `uwp_core` cria um disco sintético próprio, sem dados comerciais, e testa
a integração do núcleo. No Linux, usando o ambiente de desenvolvimento local:

```sh
dev cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
dev cmake --build build/linux --target uwp_core_test sound_test cheat_test --parallel 2
dev ctest --test-dir build/linux -R '^(uwp_core|sound|cheats)$' --output-on-failure
```

Isso não verifica AppContainer, MSVC, FXC, áudio/vídeo WinRT ou instalação.
Para a próxima etapa: executar o workflow/build Windows, instalar no Series S,
testar disco próprio e trechos gerados correspondentes, medir tempo de frame e
memória sem depurador e exercitar suspensão/retomada, controle desconectado e
salvamento após fechar o aplicativo.
