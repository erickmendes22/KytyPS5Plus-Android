# Demon's Souls — auditoria reproduzível da decisão V3

## Estado preservado

Continuação da build **#744**, revisão
`470b589f276d8e721ebe09a4136d62ab05c6dc14`, e da staging
`b20656367417fe76c51527e749029faca1fd939b`.

**G0/P8 continua BLOCKED; intervalo potencial [0,16]; nenhuma metade global
eliminada.** A primeira queda de população seleciona a janela de captura. Ela
não estabelece, sozinha, a primeira saída incorreta do jogo. Os certificados
locais abaixo não são certificados cumulativos de C1..C8.

Fontes de dados: ZIPs `20261126-113031` (#741) e `20261126-115532` (#744),
com o ZIP #737 e o crash #740 preservados como histórico. Não foi trocada a
testemunha por W0/W1, nem reaberto um ramo por associação.

## Ciclo reconstruído dos eventos executados

| Evidência | #741 | #744 |
|---|---:|---:|
| Draw que inicia o ciclo por clear executado | 40367 | 39624 |
| Frame host do início | 232 | 233 |
| Draw da primeira perda | 40979 | 40091 |
| Frame host da perda | 233 | 233 |
| Draws preparados/executados | 50 | 50 |
| Depth / material / auxiliar | 27 / 22 / 1 | 27 / 22 / 1 |
| Draws indiretos | 2 | 2 |
| Eventos retidos | 10.566 | 10.875 |
| Primeira / última sequência | 32323 / 42888 | 31358 / 42232 |
| Lacunas na sequência retida | 0 | 0 |

Em ambos os inícios, `draw_inputs.depth_load_clear=false` e
`draw_execute.depth_clear=true`. Na #744, o evento executado é seq39754.
Portanto, usar o flag preparado ou um frame host fixo perde a fronteira.
A pré-história evictada antes da janela não é uma lacuna interna da janela.

As âncoras completas dos dois ZIPs são idênticas byte a byte:

- Antes: SHA-256
  `c139cb8d96226197da46f99d3215acd48ba347ec7ae0188caa148ab7672b104f`.
- Depois: SHA-256
  `bec965c55f62ec1384f3d3338fc746b39d7079f08cd15a4b65fffc8226311342`.

O draw de fundo perde 1.439.331 pixels coloridos: 1.439.989 antes e 658
depois. O hash de palavras depois, `13258164644348433546`, é o mesmo no
consumidor. Essa identidade fixa a observação; não inventa a cor esperada.

## Certificados locais reproduzidos

### Raster/depth do fundo

`tools/prelight_sky_replay.py` lê diretamente ZIP ou diretório, confere
identidades dos módulos original/instrumentado, índices, posições,
viewport/scissor e os snapshots D32 completos. O modelo usa as posições
XYZW nativas capturadas, clipping homogêneo e amostras no centro do pixel.
Não é um replay independente do VS guest.

Resultado #744, viewport `[0,900,1600,-900,0,1]`, sem depth bias:

- 19 vértices únicos, nove triângulos originais.
- Cobertura calculada: 1.440.000 pixels.
- Máscara prevista e observada: 1.439.331 pixels alterados, 669 preservados.
- Divergências na máscara: **zero**.
- Erro absoluto máximo D32: `2.3746158825898016e-9`.
- Posições/clip do material e prepass: idênticas.
- Palavras `0xff000000` no albedo depois: 1.439.331.

A tolerância numérica do script é um critério explícito de análise, não um
limite de conformidade Vulkan nem prova de precisão bit a bit. A ordenação
de bordas/subpixel do hardware e a expectativa do foreground não são
certificadas. `tree_decision` permanece BLOCKED mesmo com resultado local
consistente. Não suprimir o fundo nem forçar profundidade/cor.

O `fingerprint` dos arquivos de vértices identifica o módulo **antes** da
instrumentação; `native_vs_file` identifica o módulo **depois**. Os arquivos
verificados contêm as instruções originais em ordem, com inserções de
instrumentação. A diferença de hash não demonstra corrupção.

### Fórmula de posição de VS868546d85ce5090a

`tools/prelight_position_formula_replay.py` compara o slice guest até
`EXP pos0` em PC `0x288` ao valor `%967` do SPIR-V
`7843cbd43b48179b`, com SHA-256 exatos de ambos os binários.

As quatro expressões ordenadas XYZW coincidem. O decoder reduzido lê os
encodings guest auditados com LLVM gfx1010; o leitor SPIR-V é independente
do IR/emissor do Kyty. **Premissas:** lane ativa e os mesmos valores já
carregados nos registradores. Não certifica fetch/endereço/versão, máscara
EXEC, precisão/FTZ, cobertura, todos os shaders ou P8.

O controle negativo altera, apenas em memória, o `OpFMul` real `%781` para
`OpFAdd`: X diverge e YZW continuam iguais. A CLI normal rejeita o módulo
alterado por fingerprint desconhecido. Nenhum controle negativo vira uma
divergência observada no jogo.

### Argumentos indiretos e queries

O replay existente `prelight_production_replay.py` reproduz 43/43 fixups
registrados na #744. Nos dois draws indiretos do ciclo, entrada e saída
são `[8907,0,0,0,0]`: `instanceCount=0`. As queries originais têm zero
samples. Esses dois draws não exigem execução de VS/índices para explicar
sua contribuição observada. O produtor guest da contagem zero não foi
certificado, portanto isso não fecha C1.

Os 50 draws têm query original. Dezessete pares foreground com replay
nativo têm a mesma cobertura prepass/material e não perdem samples por
discard nesse replay. Reexecução do mesmo SPIR-V não é oráculo guest.

## Lacunas quantificadas

Cada ciclo declara **3.304.608 bytes de buffers ligados não exportados**,
contando usos repetidos. Excluindo os dois indiretos de zero invocações,
o gate ainda exige **3.214.628 bytes ausentes**. Faltam também 269.592
palavras de índices de draws diretos. Contagem e digest não recuperam
esses arrays.

Dos 100 registros de stages VS/PS: 40 nomeiam módulos presentes, 20
nomeiam arquivos ausentes (seis caminhos únicos), 39 têm nome vazio e um
corresponde à ausência legítima de PS. São 17 VS e 19 PS distintos; somente
16 dos 36 binários guest estão disponíveis. Excluindo os quatro shaders dos
dois indiretos sem invocações, permanecem 16 shaders guest necessários
ausentes. Entre os ausentes estão:

- VS `557d65da5b786842`, usado por 15 draws de material.
- PS `afd9334e8581cda2`, usado cinco vezes.
- PS `e31a5473e9ee7503`, usado nove vezes.

Faltam ainda imagens/subrecursos efetivamente amostrados, versões,
estados de todas as operações e saídas completas relevantes de C1..C8.
Não se pode certificar um prefixo inteiro a partir dos shaders de fundo.

Os filtros antigos de `RecordDsProbeGuestCode`/`RecordModule` não capturam
todos os gráficos do ciclo; ampliar só o ZIP não recupera um módulo nunca
gravado. O exportador reserva 16 MiB fora de queue-state, mas ainda aplica
caps de 48 SPIR-V/96 guest/24 pipelines e não verifica o fecho dos arquivos
obrigatórios. O orçamento potencial de queue-state (48 MiB JSON, snapshots
D32/albedo e journal) pode superar seus 64 MiB.

A staging também acrescentaria pelo menos 2.190 readbacks síncronos de
buffers maiores que 1 KiB na janela já retida da #744, 39.119.640 bytes,
antes da perda. Isso não inclui a pré-história evictada nem os readbacks
menores existentes. Uma futura captura completa precisa agrupar/limitar
essas cópias e medir a perturbação; não tratar a serialização diagnóstica
como certificado de sincronização de produção.

## Correção adicional da staging

O payload do draw atual agora fica pendente fora do orçamento do ciclo
anterior. Depois da aquisição dos render targets, o clear executado decide
o início; só então o armazenamento reinicia e admite o primeiro payload.
Antes, um ciclo cheio podia rejeitar esse payload e só depois reconhecer
o novo clear, mantendo novamente um caso incompleto.

`CycleInputRetention` é a política usada pelo renderer e pelo teste de
regressão. Os testes reproduzem orçamento anterior esgotado para as duas
identidades #741/#744, retenção do primeiro registro novo, overflow
reportado sem expulsão silenciosa e primeiro registro oversized bloqueado.
`witness_input_retention` registra a decisão efetiva. O campo
`cycle_first_draw_before_acquire` é explicitamente apenas o estado anterior;
o evento de retenção e o manifesto fornecem a fronteira autoritativa.

É correção da instrumentação, **não correção da iluminação**. A validação
host dessa política e a revisão do diff não substituem compilação Android.

## Gate de prontidão e condição para próxima build

`tools/prelight_v3_gate.py` reconstrói o ciclo, confere a sequência,
identidades, manifesto, recursos, argumentos, módulos/fingerprints e
integridade dos arquivos efetivamente presentes. Ausência, truncamento ou
operação sem referência ficam BLOCKED. Não aceita `--oracle` com booleanos
autoafirmados ou uma cor arbitrária como certificado. O digest do conjunto
inspecionado identifica a evidência usada pelo relatório.

**Este gate de prontidão não contém um backend integral de replay C1..C8.**
Ele declara essa ausência e retorna código 2, sem reduzir [0,16] ou
autorizar um APK. Isso é intencional: rejeitar captura insuficiente não é
ter implementado o oráculo ausente. Um backend futuro precisa consumir os
operandos reais com integridade/versionamento e ser validado com paridade
e mutação causal em uma testemunha real completa. Um contraexemplo local
suficiente pode decidir FAIL; uma coleta apenas maior não decide PASS.

Comandos reproduzíveis, executados da raiz do repositório:

```bash
python3 docs/android/tools/prelight_v3_gate.py /caminho/diagnostico.zip
python3 docs/android/tools/prelight_sky_replay.py /caminho/diagnostico-744.zip
python3 docs/android/tools/prelight_position_formula_replay.py --capture /caminho/diagnostico-744.zip --self-test
```

Validação desta revisão: 23 testes do gate (incluindo os ZIPs reais #741 e
#744), 14 testes do replay raster, comparação simbólica com mutação real e
teste C++ da retenção compilado com C++20/`-Wall -Wextra -Werror` passaram.
Esses testes verificam os contratos descritos; não certificam um backend
integral ausente nem gameplay no aparelho.

Não há neste workspace executável/assets do jogo nem uma sessão do aparelho
que permita reconstituir os bytes nunca gravados. Acesso ao GitHub/Actions
está funcional; o bloqueio da decisão é a evidência/oráculo, não a fila de
build nem a assinatura. A #744 foi baixada e sua assinatura/revisão foram
reverificadas como baseline, sem apresentá-la como uma nova correção.

Não houve nova build Android nesta auditoria. A staging permanece separada
da branch que dispara o workflow. A próxima publicação de APK continua
condicionada à suficiência da captura e do oráculo do **mesmo G0/P8**.
