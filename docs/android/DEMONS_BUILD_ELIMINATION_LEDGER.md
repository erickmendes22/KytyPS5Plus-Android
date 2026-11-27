# Demon's Souls — registro de eliminação causal por build

Atualizado em **26/11/2026**. Este registro responde, para cada build: **o que
foi cortado, com qual evidência, o que não foi cortado e qual deficiência a
próxima aquisição corrige**. Uma nova build ou mais arquivos não contam como
eliminação causal.

A [árvore V3](DEMONS_PRELIGHT_BINARY_TREE.md) continua sendo o roteiro. Seu
intervalo global permanece **[0,16]**, com a primeira divisão **P(8) ainda
não decidida**. Os resultados locais abaixo reduzem operações concretas da
mesma testemunha; não podem ser somados e apresentados como metades globais
eliminadas.

## Deficiência metodológica identificada após a entrega da #746

O preparo validou um comparador local de leitura/emissão, mas não um mecanismo
capaz de decidir os dois resultados de **P(8)**. MATCH dos 60 bytes deixa o
produtor aberto. DIVERGENCE pode provar um erro pré-iluminação, mas só decide a
localização causal do sintoma quando seu efeito no mesmo episódio é demonstrado.
O vínculo de batch/targets/ordem existe; cobertura esperada do cenário com as
entradas do ciclo zerado ainda não foi demonstrada.

A [auditoria posterior da #745](DEMONS_APK_745_RESULTS.md) também distingue a
perda de albedo no frame 202 da amostragem final da cena: 64/64 pontos iluminados
no frame 209, queda para 1/64 no 233 e trigger no 281. A cadeia de versões e
contribuições entre esses eventos não está fechada. Isso é lacuna de vínculo,
não prova de que se trata de outro defeito nem autorização para trocar o caso.

Além disso, `tools/prelight_v3_gate.py` inicializa e conserva P8=BLOCKED; não
implementa o comparador global. **A falta de corte não pode ser atribuída somente
à espera pelo próximo ZIP.** O programa audita a captura e anexa provas locais.
Não basta reclassificar seus resultados, tornar arquivos obrigatórios ou remover
uma mensagem de lacuna para implementar P(8).

Correção de foco: a próxima build diagnóstica deve fornecer aquisição e
verificador executável dos dois desfechos do nó ativo. Isso é um requisito do
instrumento a construir, não a exigência circular de já possuir a resposta do
aparelho. Reutilizar os certificados e analisar os dados da #746 quando
disponíveis; não contabilizar a captura local como uma decisão global.

## Atualização #746 / #747 — cobertura da fronteira P8

Diagnóstico real #746: `20261126-144336`, 404/404 dependências obrigatórias e
649 entradas do inventário verificadas. A primeira perda de albedo é no
frame 217; o journal termina no 222. Isso não certifica o frame de gameplay
escuro posterior. No frame 481, o log registra **40 índices de filas que
escrevem HDR** (0–31 e 33–40), além de passes gráficos e pós-processamento.
Nem todos são necessariamente consumidores do G-buffer; a associação deve
ser determinada pelas bindings efetivamente usadas. O compositor consome
0x281a80000 (overlay) e 0x2b2a40000 (cena pós-processada). No frame 481 as
cinco amostras do segundo recurso têm R=G=0 e luminância muito baixa, mas
não são uma referência independente do material esperado.

A #746 capturou 12 registros CP em quatro batches; o mecanismo de referência
direta respondeu `pipeline_or_front_thread_unsupported`. Não se promove
essa falha de aquisição a uma divergência de execução. A comparação local
de publicações/consumo relatada na análise anterior não corta P8.

#747, commit 63ea76b1f3fcfb4f506322aceed71ab7ef786dfc, run 45881244220:
candidato de desenvolvimento de replay Vulkan, não build de decisão P8.
Os testes rápidos executam igualdade e divergências nativas, mas o
comparador continua sem referência guest e limita a cobertura explícita a
0, 8 e 33. A compilação ainda não constitui um corte. Não pedir ao usuário
para instalar como se a decisão global estivesse preparada.

Revisão staging seguinte: censo de bindings reais de todos os dispatches
normais diretos/indiretos e draws admitidos na janela, marcação de cada uso,
captura dos subrecursos material/depth efetivamente vinculados e reutilização
de uma cópia apenas para a mesma identidade/versão limpa. Isso corrige a
dependência de três hashes, mas não certifica a referência no momento de
cada uso nem cobre automaticamente caminhos especializados. A substituição
causal e a referência independente da tradução guest continuam pendentes.
**Corte global: nenhum; P8 permanece aberto.** Não promover staging ao
branch de APK enquanto esses requisitos de decisão não forem satisfeitos.

## Reanálise offline #746 — inicialização dos argumentos indiretos

Binário guest CS 71e0e2994d683e35, SHA-256
09267e1f60812eb432c306a9e3fc6bb2f5c451f8fe8958404152579873a213af.
A decodificação LLVM gfx1010 dos PCs 00a4, 0650, 065c e 06f0..0798,
com SRT consumido, blob de operandos preservado (917504 bytes, FNV1a64
3120090459754847557), grupos [1,1,1] e dois lanes ativos, reproduz os
índices de registros 1 e 4. As três estruturas de draw em 0x21de00078
são inicializadas independentemente da expressão de visibilidade: todos
os 15 DWORDs coincidem com a observação imediatamente após o dispatch
e com o consumo posterior. O contador dinâmico de dispatch (offset +0
do registro) não está incluído nesse certificado.

A inicialização não prova que os argumentos deveriam permanecer zerados.
Os produtores seguintes, seq 4923 (b213157ad251f2c7), 4932
(7d5ad40fca3137e2) e 4941 (ea2e66ea6ad76baf), mantiveram a janela nativa
de 60 bytes sem alteração. Seus operandos não foram retidos precisamente
por não terem alterado o destino. Isso inviabiliza verificar a ausência
de uma escrita esperada. A publicação seq 4949 e o batch seq 4954
preservam os mesmos 60 bytes; não se atribui ao reset a causa do sintoma.
Correção de aquisição: manter uma cadeia limitada por versão, incluindo
candidatos sem alteração, com blob individual e omissões explícitas.
A cadeia continua subordinada à fronteira P8; não autoriza uma build
isolada de CP nem um corte global. A referência completa ainda falta.

**Corte global: nenhum.** CI do exportador no commit eceefeb:
run 45891061642 concluído com sucesso. A captura por consumidor torna
obrigatórios os arquivos guest antes descartados pelo limite opcional.

## Staging — transferência distinta para cada uso da fronteira P8

O verificador causal rejeitava a reutilização de snapshots de outro submit,
enquanto o capturador deduplicava a própria transferência. Corrigido o
contrato: cada uso recebe uma cópia Vulkan marcada e observada; apenas o
arquivo de bytes realmente iguais pode ser compartilhado, após comparação
exata, com orçamento separado de cópias e armazenamento. O verificador
vincula o marcador de consumo à ordem real da captura, rejeita transfers
reutilizados e nomeia a referência independente pelo marcador (não pelo
arquivo Android deduplicado). Testes exercitam dois submits com bytes
iguais, referências distintas, marcador ausente/tardio, reutilização
inválida, orçamento e corrupção do arquivo deduplicado.

O ajuste evita uma incompatibilidade entre aquisição e replay. Ainda não
é uma referência guest para o G-buffer completo. **P8 continua aberto.**

## Como registrar um corte

| Tipo | O que a evidência permite afirmar |
| --- | --- |
| Operação local certificada | Para o comando, versão, inputs e outputs identificados, essa operação não é a primeira divergência demonstrada. Seguir sua origem ou a próxima aresta ainda aberta da mesma cadeia. |
| Divergência local comprovada | A operação não preservou os bytes ou o contrato esperado para aquela mesma entrada. Fixar o contraexemplo, localizar a primeira transferência/instrução divergente e implementar a correção. |
| Corte global de P(8) | A evidência justifica selecionar uma das metades da árvore no cone causal do sintoma. Um MATCH isolado de VS, raster ou count não produz esse certificado. |
| Revalidação | A operação já estava certificada; a execução nova confirma o resultado com seus próprios operandos. Conservar a prova, sem contabilizar novo ramo cortado. |

Não se exige replay integral, captura externa de PC ou rastreador universal
de stores CPU para decidir uma aresta local. Uma captura incompleta também
não autoriza trocar de testemunha ou reabrir operações certificadas.

## Quadro de progresso

| Build / evidência | Redução local efetivamente obtida | Novo corte global P(8) | Deficiência que explica a ausência de corte global | Ação concreta |
| --- | --- | --- | --- | --- |
| **#744**, ZIP `20261126-115532`, núcleo `470b589f276d8e721ebe09a4136d62ab05c6dc14` | Certificados locais do fundo e dos fixups reaproveitados; posição do fundo reproduzida offline. A fórmula simbólica de VS8685 tem escopo limitado aos mesmos valores já carregados. | **Nenhum.** | A captura não continha os operandos/índices/módulos necessários para comparar numericamente as posições do foreground. Um replay do mesmo SPIR-V e uma fórmula simbólica não supriam esses inputs. | A #745 corrigiu seleção pelo clear executado, captura dos inputs realmente consumidos, posições e fecho de exportação. O resultado dessa correção foi verificado no ZIP seguinte. |
| **#745**, ZIP `20261126-132544`, núcleo `4a94749e889ffd178418d347fcdd82ccc83ca73e` | **45 POS0 reais comparáveis** coincidem com a execução independente da ISA guest: 73.580 vértices / 294.320 componentes bit a bit. O slice de posição dessas invocações foi retirado do conjunto de operações divergentes. | **Nenhum.** | A fronteira seguinte é o batch CPU de três registros em `0x21de00078`. Seus **60 bytes e sua versão independente no instante do consumo não foram adquiridos**. O journal demonstra a mudança de count, mas não decide leitura correta versus bytes de outra versão. | A #746 implementa comparação por byte entre consumo preservado e referência independente, emissão separada, retenção dos extremos ligada à perda e cópias GPU coletadas depois dos três consumos. |
| **#746**, commit `4e321c2dbedcab17085c1af6ecf0d0f4f8345afe`, run `45866254600` | **Nenhum resultado de gameplay/captura ainda.** Há mecanismo local READ/EMIT implementado e testado antes da build. | **Nenhum registrado.** | **COMPILADO, ASSINADO E VERIFICADO / AGUARDA_CAPTURA**. Além da captura pendente, o preparo não fechou um comparador bilateral de P(8); um MATCH local não elimina uma metade global. | Aproveitar o diagnóstico quando disponível e registrar seu alcance. Antes de outra build diagnóstica, completar o contrato de aquisição e o verificador executável de P(8), com os dois desfechos e o vínculo ao sintoma. |

Run da #746: [GitHub Actions 45866254600](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45866254600).
A compilação e as verificações do APK terminaram com sucesso:
[registro da entrega](DEMONS_APK_746_VERIFICATION.md). O resultado no aparelho
permanece pendente e não foi inferido dos testes de software.

## #744 — o que foi aproveitado e por que não resolveu a divisão

O ciclo correto é **39624→40091**, selecionado pelo clear **executado**. A
[auditoria reproduzível](DEMONS_V3_AUDIT_2026_11_26.md) reconstruiu 50 draws
e nenhuma lacuna interna no journal retido. Esse ciclo não deve ser
reiniciado nem substituído por um frame host escolhido arbitrariamente.

Os dados sustentam os seguintes certificados locais:

- O replay raster do fundo reproduz a máscara D32 sem divergências:
  **1.439.331 pixels alterados e 669 preservados**. O erro numérico máximo
  de depth é `2.3746158825898016e-9`, sob o contrato declarado do replay;
  isso não é uma alegação de conformidade bit a bit do hardware.
- A execução independente dos dois VS do fundo reproduz **38 vértices /
  152 componentes XYZW bit a bit**, conforme a
  [validação anterior à #745](DEMONS_APK_745_VERIFICATION.md).
- **43/43 fixups** registrados coincidem com seu contrato. Nos dois
  indiretos do ciclo, os argumentos `[8907,0,0,0,0]` significam zero
  instâncias; não há VS a executar para essa contribuição observada.
- As expressões simbólicas XYZW de VS8685 coincidem sob a premissa de
  mesmos registradores de entrada e lane admitida. Essa prova não contém
  os fetches, versões e valores numéricos ausentes dos draws foreground.

**Nenhuma metade global foi cortada.** A deficiência técnica era concreta:
faltavam **3.214.628 bytes de buffers** mesmo após excluir os indiretos sem
invocações, **269.592 palavras de índices diretos** e shaders necessários.
O seletor podia perder o primeiro input antes de reconhecer o novo clear,
e o exportador podia remover dependências. Acrescentar mais replay do céu
não resolveria essa ausência.

A correção executada na #745 foi adquirir esses operandos e comparar as
posições pela ISA guest, preservando os dados reais consumidos. Nenhum dos
certificados acima autoriza forçar cor, depth, visibilidade ou contagens.

## #745 — avanço novo e revalidações, sem dupla contagem

O ciclo correspondente é **55824→56618**. Os 50 inputs, os 45 arquivos de
posições e **375/375 dependências obrigatórias** chegaram íntegros. As
extensões de opcodes necessárias foram resolvidas **offline no mesmo ZIP**.
Não houve uma build por opcode. Os números e as identidades estão em
[DEMONS_APK_745_RESULTS.md](DEMONS_APK_745_RESULTS.md).

### Corte local obtido

**A entrada consumida produz o mesmo POS0 no guest e no módulo nativo para
todos os 45 draws comparáveis.** São 73.580 vértices e 294.320 componentes,
sem diferença de bits. Isso retira o fetch/cálculo de posição dessas
invocações como primeira operação divergente, dadas as entradas comparadas.
Não certifica os produtores anteriores dessas entradas, nem invocações que
não foram emitidas.

Os cinco draws restantes não foram escondidos dentro de um PASS geral:

| Draws | Classificação | Efeito sobre a investigação |
| --- | --- | --- |
| 56022 e 56031, VS233719 | Referência ISA calculada; posições nativas sem instrumentação suportada. | Não são um contraexemplo. Não trocar o caso pelo Z literal zero desse shader. |
| 56040 e 56603 | Source/fixed `[8907,0,0,0,0]`; zero invocações. | A contribuição observada não exige execução de VS. A origem dos argumentos não é certificada por esse zero. |
| 56387 | Referência auxiliar calculada, sem observação nativa; operação sem escrita de albedo/depth/stencil. | Não inventar uma lacuna de cor do cenário nessa operação. |

**Saldo inequívoco: 45 operações POS0 certificadas nesta execução; zero
ramos globais P(8) cortados.** O conjunto de 45 inclui a revalidação do céu.
A expansão da comparação aos demais draws é o avanço da aquisição; não se
atribui uma contagem líquida de 45 cortes inéditos ignorando certificados
anteriores.

### Provas preservadas, sem contabilizá-las como ramos novos

O céu foi recalculado com os operandos da #745: **1.439.342 pixels de depth
alterados**, complemento exato dos **658 pixels RGB não zero** do albedo.
Os **34 fixups** e as **32 operações cb3b** dessa captura também coincidem
com seus contratos. São revalidações das operações já certificadas. O
certificado do céu já está incluído no conjunto de posições; não deve ser
somado como outro corte de POS0. Esses números não significam
`45 + 34 + 32` novos ramos globais eliminados.

### Por que P(8) não foi cortado e o que muda na #746

**A divisão global continua sem corte.** O journal mostrou que o mesmo
batch de cenário usava 98.304 índices no filho0 do comando **54823** e
passou a zero nos comandos **55986/56594**, com uma instância. A falta
restante para a próxima comparação local é a fonte dos três registros de
20 bytes em **`0x21de00078..0x21de000b4`**, com a versão independente
correspondente às leituras originais.

Esses 60 bytes são a **lacuna mínima demonstrada na próxima aresta da cadeia
observada**, não o único obstáculo possível à certificação global de P(8).
Mesmo um MATCH dessa leitura deixa a produção dos bytes como próxima
dependência a verificar. A aquisição local não é vendida como garantia de
corte global.

O fallback CPU já existia quando havia geometria. Uma leitura guest pode
disparar o fault handler e publicar bytes; portanto `cpu_fallback` e count0
não demonstram leitura desatualizada. Os registros de f0c140 em outro
endereço também não substituem essa fonte. Não existe evidência para
selecionar iluminação ou trocar de galho a partir dessas ausências.

A #746 corrige a aquisição dessa **mesma aresta**, em vez de repetir os
vértices certificados. Captura também as transferências e os operandos de
produtores observados, para permitir avançar mais de um nível no mesmo
diagnóstico quando esses dados forem suficientes. Não há promessa de que
uma referência local certifique automaticamente todos os ancestrais.

## #746 — contrato local READ/EMIT antes da captura (histórico)

Pergunta local: **a versão independente dos 60 bytes coincide com os valores
efetivamente lidos e com os argumentos realmente emitidos pelo CP?** O
[contrato implementado](DEMONS_CP_INDIRECT_CAPTURE.md) fecha as deficiências
que impediam responder a essa pergunta:

1. Preserva os cinco DWORDs e cada uso original antes de qualquer espera
   diagnóstica. Conserva o tuple emitido separadamente.
2. Escolhe a referência **por byte**: ledger GPU-dirty/publicação pendente
   usa o buffer nativo existente; bytes CPU usam o backing por alias separado.
   Um espelho Vulkan antigo de página CPU-dirty não é tratado como esperado.
3. Congela cada referência antes do filho seguinte e só recolhe as cópias
   GPU **após os três consumos**, em staging dedicado. O estado usado na
   decisão é o do enqueue, separado do estado após a espera.
4. Mantém último não zero e primeiro zero móveis até a perda observada;
   registra `observed_loss`, `sealed` e `last_seen`. Um positivo novo antes
   da perda substitui o candidato antigo. Os extremos e seus blobs têm
   retenção independente do ring dos 50 draws.
5. Exporta eventos, identidades, epochs, publicações e dependências pinadas,
   com tamanhos/digests e perdas explícitas. O epoch avança mesmo quando um
   sinal não consegue reservar slot no ring.

Antes do disparo foram validados **40 testes do auditor indireto**, a política
C++ com warnings tratados como erro e o exportador Java real (**79 assertions
de fixtures e 12 sobre os ZIPs existentes**). Há controles positivos e
negativos de igualdade, consumo alterado, emissão errada, pending com dirty
vazio, propriedade mista, GPU antiga, coleta antecipada, mudança de versão,
pai/range incorretos, arquivos ausentes/truncados e drops. Isso valida o
mecanismo; **ainda não é resultado da execução da #746 no aparelho**.

### O que cortar e como continuar para cada resultado

| Resultado efetivo do auditor | Corte local ou constatação autorizada | Próxima ação na mesma cadeia |
| --- | --- | --- |
| **MATCH** entre referência, leituras e emissão | Eliminar leitura/decodificação/emissão **como explicação de uma diferença entre esses bytes e esses comandos**, dentro da versão e janela comparadas. Count0 também pode dar MATCH. Não eliminar o produtor ou publicações anteriores não comparadas. | Seguir o produtor/transferência preservado no mesmo ZIP. Reproduzir sua operação com os inputs retidos; não repetir POS0, céu, fixup ou cb3b já certificados. |
| **DIVERGENCE de leitura contra bytes GPU autoritativos**, identidade e ordem estáveis | Rejeitar a hipótese de que o CP consumiu fielmente essa versão. Fixar registro, endereço/DWORD, esperado e consumido. Operações posteriores não explicam essa diferença já observada. | Usar backing/publicação/fault/transferências para localizar a primeira passagem divergente; implementar correção dessa operação e teste de regressão. Não forçar count nem atribuir o zero a decisão guest legítima antes de resolver a divergência. |
| **DIVERGENCE na emissão**, leituras e referência iguais | Eliminar falta de fonte ou leitura errada como explicação desse tuple. O contrato de min/count, índice/base/offset ou instância divergiu entre entrada e comando emitido. | Corrigir a operação de decodificação/emissão identificada; confrontar o filho com o journal quando retido e revalidar o caso. |
| **MATCH parcial**, outro record INCOMPLETE/UNSTABLE | Cortar somente as operações dos records efetivamente certificados. Não descartar suas provas por causa do outro record. | Usar os dados já presentes para resolver a lacuna restante; manter pai, filho, range e versão fixos. |
| **INCOMPLETE** por arquivo, bytes, identidade ou referência ausente | **Nenhum corte para o record incompleto.** Informar exatamente o item faltante e a causa da falha da aquisição. | Corrigir retenção/exportação/associação ou o suporte específico que falhou; exercitar um fixture desse defeito antes de outro APK. Não pedir repetição sem corrigir a causa concreta. |
| **UNSTABLE** por mudança entre consumo e snapshot, ou diferença tardia em página CPU-dirty | **Nenhum corte para essa comparação temporal.** Uma escrita concorrente não é falha retroativa de leitura. | Examinar epochs, ordem e produtores já capturados. Se não bastarem, a próxima aquisição deve corrigir a janela ou ligação de versão desse range, sem exigir um trace CPU universal e sem trocar de galho. |
| Perda não observada ou extremos não vinculados ao episódio | Preservar comparações locais válidas; **nenhum corte atribuído ao sintoma** sem o vínculo necessário. | Conferir `observed_loss`, slots e `last_seen`; resolver seleção/retenção específica antes de repetir o teste. Não escolher outro draw apenas por ter count0. |

Uma divergência local só altera a metade global quando seu vínculo causal
com o sintoma sustentar essa conclusão. A obrigação imediata é aproveitar o
contraexemplo e corrigir a operação comprovada, sem abrir novas sondas em
galhos paralelos. Um MATCH encaminha diretamente para a origem preservada;
não autoriza encerrar a investigação na comparação concluída.

## Atualização obrigatória após cada captura

O registro da build deve ser completado antes de propor outra:

- ZIP, commit embarcado, modo e identidade do episódio analisado.
- Resultado de cada aresta avaliada, incluindo os records ausentes/instáveis.
- **Operações novas eliminadas**, separadas de revalidações anteriores.
- **Ramo global cortado, ou “nenhum”**, com a razão direta.
- Primeiro contraexemplo, quando existir, com bytes/versão/operação e correção.
- Quando nenhum corte novo tiver sido obtido: causa concreta da insuficiência,
  correção da aquisição e teste que reproduz a deficiência; indicar a decisão
  executável que a próxima build deverá permitir. Não registrar apenas
  “investigar mais”, nem garantir como fato um resultado ainda não observado.

Nenhum item desta lista exige uma referência global pronta para adquirir os
operandos de uma comparação concreta. Ela exige prestação de contas sobre o
avanço real de cada build e continuidade da mesma cadeia causal.

## Staging — referência guest independente (26/11/2026, após #747)

**P8 permanece aberto, [0,16], nenhuma metade global eliminada.** Não promover
uma nova build Android até implementar os dois desfechos do instrumento.

- #746: produtor ea2e66ea6ad76baf, frame 215, submit 6292, seq 4891/4892.
  O blob retido de 1.638.400 bytes tem FNV1a64 2840090357011714871. Os
  quatro segmentos reconstruídos totalizam 1.638.428 bytes (incluindo SRT
  fora dos buffers); os 120 bytes anteriores aos dispatches conferem
  exatamente. Os IDs de record são 2,3,1. A reprodução matemática do
  guest gfx1010 com a ordem legal de atomics 2,3,1 gera os mesmos 120 bytes
  posteriores, inclusive firstIndex 442536 e 344232. Não prova que outra
  ordem seja errada, nem que a compactação do frame 216 esteja correta.
- No produtor de culling b213157ad251f2c7, entre seq 4876 (frame 215) e
  4917 (frame 216), os 16 DWORDs da matriz em 0x2088800d0 mudaram.
  Portanto os índices do frame 215 não constituem oráculo do frame 216.
  Os dois shaders b213157ad251f2c7 e 7d5ad40fca3137e2 realmente executam
  image_sample_l da imagem HZB 0x2b2570000; os bytes de entrada do
  episódio zerado não foram preservados na #746. Não imputar o zero
  à tradução sem os operandos daquela execução.
- O frame 481 de gameplay escuro apresenta material não nulo e depth
  não nulo na amostragem da fronteira. A queda anterior de albedo não
  certifica o mesmo cone da saída final. Preservar suas provas locais.
- Staging cdf24b2d: executor de ISA original com AMD Rocjitsu, upstream
  431f58f7841a88b705992a91d9d71fd972345beb (RDNA1/RDNA2), sem passar
  pelo recompilador de Kyty. Workflow 45897822805 em execução à
  época deste registro; não registrar compilação como aprovação antes
  do resultado. O primeiro adaptador suporta uma wave, sem LDS; os
  testes de memória e a calibração local não certificam rasterização,
  imagens guest ou o prefixo inteiro.
- O replay Vulkan causal 45897822678 passou, inclusive transferência,
  controles e compilação do replay consumer. Falta conectar a execução
  guest ao conjunto de dependências de todos os marcadores P8, com
  estado de rasterização e versões dos recursos, para que o crossover
  compare resultados realmente independentes. Não substituir isso por
  um indicador booleano no manifesto.

Reprodução offline (somente host):
`python3 docs/android/tools/p8_prepare_guest_capture.py KYTY_746.zip OUT --seq 4891`.
O arquivo observado fica separado dos bytes do manifesto de execução.
A referência não usa o conteúdo do target posterior como entrada.

## #748 — aquisição integrada da fronteira P8 (preparação)

A análise offline executou o binário guest real ea2e66ea6ad76baf da #746,
frame 215 / submit 6292, com o executável Rocjitsu efetivamente compilado
no run 45897822805 (artifact 15156142536, SHA-256 do ZIP
056fb5eda9256b446b1e4256edecd2954a281a8f790b75ff08bdfbc11a72b9dd).
**78 passos, 45 acessos arquiteturais, 120/120 bytes iguais** nos ranges
0x21de00078 e 0x21de00138; zero diagnóstico de execução. Preservar esse
certificado local; não comparar a câmera do frame 215 com a do 216.

O problema de aquisição seguinte está demonstrado: image_sample_l dos dois
produtores de visibilidade usa HZB, mas a #746 preserva somente os descriptors.
O mesmo blob agora inclui os bytes reais de todos os subrecursos lidos pelos
dois produtores, em ordem anterior ao dispatch, com descriptor, view/swizzle,
extent, versão, offsets e digest. A retenção e exportação seguem os mesmos
endpoints dos argumentos, com 16 MiB por geração e 64 MiB globais; o teste
de política exercita limites e overflow. Isso alimenta a execução guest e
o crossover de P8; não força contagem, iluminação ou depth.

**Corte global: nenhum.** #748 é a aquisição integrada dos mecanismos
preparados; não exige ter o gameplay futuro ou um PC externo para compilar.
Ainda não há prova integral de P8: o executor de wave não é rasterizador,
e o replay Vulkan não é referência guest independente. Após o diagnóstico,
comparar os dados da fronteira, usar o crossover control/sham/substitution
com referência válida e só classificar PASS/FAIL no alcance demonstrado.
Não publicar resultado fictício se a captura ou uma comparação falhar.

## #748 — entrega verificada (26/11/2026)

APK compilado, assinado, baixado e verificado: revisão `0f1709763e30ab7812652d2eda81518a6e32ebff`, run [45905112907](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907), artifact [15157339392](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907/artifacts/15157339392). Registros de assinatura, integridade, ABI e alcance em [DEMONS_APK_748_VERIFICATION.md](DEMONS_APK_748_VERIFICATION.md). **Corte global: nenhum. P8 [0,16] permanece ativo; aguarda captura F do aparelho.** A entrega não fica condicionada a PC externo ou ao resultado futuro de gameplay.

## #748 real / #749 — congelamento da aquisição Vulkan

ZIP 20261126-212515: frame 380, luminância 0.000041, 0/64 pixels iluminados; depois nenhuma apresentação nova e filas pendentes. PID 15361 RSS 9.109.968 KiB, saída code 1, sem assert nativo registrado. Export-manifest: witness_trim_trigger.gfxr 14.218.357.063 bytes, capture-status.json 148 bytes, ambos recusados pelo limite de 1,5 GiB. A serialização do estado Vulkan inteiro é a regressão de aquisição demonstrada; o ponto exato do bloqueio não está amostrado. #749 não instala nem dispara essa camada no modo F. Preserva captura raw de um frame, com 192 MiB de armazenamento e 256 MiB de transferências; observações da mesma identidade/versão limpa podem ser reutilizadas com proveniência explícita, nunca como transferências independentes de replay. Exporta metadados e arquivos menores mesmo se um trace enorme for rejeitado. Não altera shaders de produção, iluminação ou contagens. Testes exercitam trigger, desativação da camada e rejeição de reutilização dirty/versão/identidade diferentes.

CP da #748: blob_used=52.416.552 bytes, zero drops, loss frame 268. Primeiro zero do stream 1 frame 267 submit 7763: seq 5268/5277/5286/5295. Ambos produtores HZB preservaram os nove mips completos, mesma versão 4052; controle seq 5220/5229/5238 versão 4034. Todos os texels são zero nos dois episódios (não atribuir a diferença de visibilidade ao HZB isoladamente). Reutilizar estes dados em replay offline. Corte global P8: nenhum; intervalo [0,16]. Ausência da captura Vulkan não é divergência guest. A #749 corrige a causa provável de congelamento da instrumentação, ainda a validar em gameplay.

## #749 — entrega verificada e replay dos operandos reais da #748

Build Android 45916982779 concluída com sucesso. Revisão embarcada 9b67c4548dc57c90beb99a623ef80458f688cd85; versionCode 849. APK 18.491.426 bytes, SHA-256 0af55dd8ada25a38065d2e7d4d68fd06def62a3b2e8a0f255019df5cf7ac0a51. Certificado de atualização 8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0. Artefato [15163086797](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45916982779/artifacts/15163086797). ZIP e APK CRC, assinatura v2 RSA PKCS1 SHA256, digest de conteúdo v2, revisão nativa e manifest, ABI ARM64 e presença do marcador raw_only_armed/ausência de start_requested verificados independentemente. Regressões rápidas, Android unit tests, assembleRelease e auditoria nativa passaram; regressões completas opcionais não executadas.

Replay host dos bytes reais da #748, ZIP 20261126-212515, backend AMD Rocjitsu: cinco dispatches, 712 workgroups, nenhum erro de execução. Comparação com a captura imediatamente seguinte, no mesmo submit e mesma alocação nativa, dos 24 DWORDs de cada record e 24 DWORDs da região de contadores globais: 240/240 DWORDs (960 bytes) iguais.

| Seq -> próximo | Shader | Groups | Record | Índices esperados/observados |
|---|---|---:|---:|---:|
| 5220 -> 5229 | 7d5ad40fca3137e2 | 6 | 2 | 344232/344232 |
| 5229 -> 5238 | 7d5ad40fca3137e2 | 1 | 3 | 98304/98304 |
| 5238 -> 5247 | b213157ad251f2c7 | 1 | 1 | 98304/98304 |
| 5277 -> 5286 | b213157ad251f2c7 | 1 | 1 | 0/0 |
| 5286 -> 5295 | 7d5ad40fca3137e2 | 703 | 4 | 1056711/1056711 |

A agregação usa os incrementos atômicos aditivos independentes por workgroup; os valores anteriores afetam a alocação dos registros de saída, cujos bytes não estão certificados. Para memória de destino somente escrita foi usada inicialização sintética, sem tratá-la como operando lido. HZB real preservado, com os nove mips zerados reempacotados em layout linear equivalente. O replay mantém os demais operandos capturados e não comprova todos os interleavings nem a origem guest dos texels.

Contraprova local b213: mantendo índices, vértices, instância e HZB, trocar apenas os dois DWORDs 0x2088806b0 (controle c1b64236/41345580, perda c2e66b7d/c3e82b54) entre os dois manifestos troca 98304 por zero e zero por 98304. A instrução de carga guest 0x0388 lê s[18:19]+0x6a0; os valores entram no cálculo do vetor centroide menos posição de referência e teste angular 0x04bc. Isso identifica dependência causal local dos operandos de culling, não um defeito confirmado nesses valores nem uma autorização para sobrescrevê-los. Reproduzir a origem dos operandos e seu vínculo com o sintoma de iluminação; preservar certificados anteriores. P8 global [0,16], zero cortes de metades. #749 corrige a regressão de captura whole-state; validação de gameplay pendente. A captura raw limitada não substitui uma referência guest integral.


## #749 real — mesma versão G-buffer/HDR, seleção temporal e exportador

ZIP 20261127-010915, revisão 9b67c45, modo F. Apresentação avançou ao frame 555; captura raw encerrada no 337 (target 335), 99.019.876 bytes. 775 usos, 304 snapshots (18 físicos, 286 reutilizações de versão), 222 binários guest referenciados presentes. Sem repetição do congelamento anterior nesta execução.

Frame 335, submit 9965, G-buffer 0x287ae0000 version 33155: 436/1.440.000 pixels RGB não nulos. Filas 0,8,33 consomem as mesmas quatro camadas; HDR tem 0,406,421 pixels RGB não nulos respectivamente. Os 421 pertencem exatamente à máscara de 436. Depth não vazio; stencil 86 coincide com o material colorido, 68 com o restante. Nenhuma escrita gráfica do G-buffer ou depth correspondente ocorre antes das três filas dentro do frame 335: a produção atravessa o limite de frame. A captura antiga terminou no 230, após perda no 225; o compositor recuperou 64/64 no 262 e 310. Não ligar a transição anterior ao gameplay escuro como se fosse a mesma testemunha. P8 global [0,16], nenhum corte de metade.

Culling #749: três estados (seq 3053,3938,3979), reconstrução independente da posição da câmera pela solução linear x=y=w=0 da matriz de projeção: erro máximo por componente 0,00001125. Os valores consumidos são coerentes com a câmera observada. Preservar replay #748; não corrigir artificialmente esses DWORDs nem assumir os mesmos índices após mudança de câmera.

Exportador: 463/463 caminhos obrigatórios incluídos, mas 1407 erros ClosedByInterruptException (1278 pipeline,124 JSON,5 journal) na validação de fechamento, mais 80 exclusões opcionais por orçamento. Corrigir leitura de arquivos regulares com streams clássicos limitados, sem apagar flag de interrupção; teste de interrupção e versão de consumidor incompatível.

Próxima compilação: selecionar três observações escuras distintas, capturar produtores no frame anterior completo e até a primeira fila 0 real do frame-alvo, congelar o ciclo na identidade/versão/ordem consumida, não na primeira perda temporal da inicialização. O fechamento do exportador verifica a identidade exata em vez de escolher outra perda do journal. Manter os limites raw #749 e a camada whole-state desligada. Não altera os shaders de produção, contagens ou a luz. Este conserto resolve a lacuna temporal demonstrada, mas não afirma P8 PASS/FAIL ou iluminação corrigida antes do replay independente dos produtores e da saída.


## #750 — entrega verificada e seleção da fronteira escura

[Registro da entrega e evidência real da #749](DEMONS_APK_750_VERIFICATION.md). Revisão 339922af, build 45939076222, APK versionCode850, assinatura compatível verificada. P8 [0,16], sem corte global: produtores 224–225 não correspondem a consumidor 335. A #750 fixa seleção temporal e transporte; preserva os certificados anteriores sem trocar de hipótese por mera contagem. Aguarda teste F.
