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

## #746 — decisão preparada, resultado ainda pendente

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
