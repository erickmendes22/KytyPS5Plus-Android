# Demon's Souls — resultado da captura #745

## Identidade e continuidade

Diagnóstico `KytyPS5-Plus-diagnostics-20261126-132544.zip`, núcleo
`4a94749`, modo F, PPSA01341, Turnip GameHub 26.3 DD GMEM e saída 1280×720.
A sessão começou em 26/11/2026 às 13:22:15, horário de São Paulo.
O recurso observado continua sendo o albedo `0x287ae0000`, layer 0/mip 0,
1600×900; o depth continua em `0x282b60000`.

O clear executado inicia o ciclo no draw **55824**, frame 201/submit 5831.
A primeira perda material é o draw **56618**, frame 202/submit 5839.
Os 50 draws têm correspondência ordenada com os 50 da #744: mesmos shaders,
contagens, topologias e flags; os 47 digests de índices comparáveis coincidem.
Constantes, buffers e posições não são todos idênticos entre as execuções.
Por isso os resultados abaixo foram recalculados com os operandos da #745.

## Aquisição efetivamente fechada

| Verificação | Resultado |
| --- | --- |
| Arquivos obrigatórios | 375/375 presentes |
| Bytes obrigatórios | 56.034.652 |
| Bytes exportados, incluindo opcionais | 113.637.515 |
| Arquivos com tamanho/SHA-256 conferidos | 608/608 |
| SPIR-V obrigatório com XXH3 e estrutura conferidos | 83/83 |
| Inputs do ciclo | 50, incluindo o primeiro draw após o clear executado |
| Posições nativas | 45 arquivos; zero registros descartados |
| Regiões escalares consumidas | 1.846; 28.768 bytes; sem conflitos |
| Buffers ligados não exportados | Zero bytes nos usos declarados |
| Journal | 12.034 eventos; nenhuma lacuna interna |

`required_complete=true` certifica o transporte das dependências declaradas.
Não foi usado como certificado automático da iluminação ou de P8.

## Arestas decididas com os dados novos

O oráculo numérico independente executou os slices guest até EXP POS0.
As extensões de ISA necessárias foram implementadas e verificadas offline;
não foi pedido outro APK para cada opcode.

**45/45 draws, 73.580 vértices e 294.320 componentes XYZW coincidem bit a bit.**
Nenhum NaN ou evento FTZ ocorre nesses slices. Isso elimina fetch/cálculo de
POS0 dessas invocações como primeira operação divergente, dadas as regiões
escalares e os buffers efetivamente consumidos. Não certifica o produtor
anterior desses bytes nem invocações que não foram emitidas.

Os cinco draws restantes também têm classificação explícita:

| Draws | Resultado e alcance |
| --- | --- |
| 56022, 56031 | VS233719 tem saída nativa sem instrumentação suportada. ISA e operandos permitem calcular 4.862 + 65 vértices offline; Z guest é literalmente zero. O módulo nativo conserva esse zero. Não confundir ausência de posição capturada com ausência de operandos ou erro de iluminação. |
| 56040, 56603 | Argumentos indiretos `[8907,0,0,0,0]`: zero instâncias e zero samples. Não exigir um VS que não executou; origem da contagem continua separada. |
| 56387 | Operação auxiliar, sem PS, sem escrita de albedo/depth/stencil; não é uma lacuna de cor do cenário. |

O prepass e o material do céu têm a mesma posição. A máscara D32 alterada
contém **1.439.342 pixels**; é exatamente o complemento dos **658 pixels**
RGB não zero do albedo final. A query original do material do céu também
retorna 1.439.342 samples. Preservar o certificado local do fundo; não mudar
cor, depth ou visibilidade artificialmente.

Os 34 fixups indiretos registrados coincidem com seu contrato. Nos 32
registros cb3b, operandos, aritmética, stores e readback coincidem. Não
reabrir essas operações por associação com outra contagem zero.

## Próxima aresta da mesma testemunha

O journal contém uma fronteira anterior concreta que os 50 draws preparados
não incluíam: os três registros de argumentos em **`0x21de00078`**, stride 20,
usados pelo mesmo batch de cenário antes da perda.

| Extremo | Comando | Submit/tick | Primeiro registro consumido |
| --- | --- | --- | --- |
| Último material com geometria | 54823 | 5794 / 38540 | 98.304 índices; firstIndex 442.536; uma instância |
| Prepass do ciclo da perda | 55986 | 5831 / 39359 | zero índices; firstIndex zero; uma instância |
| Material do ciclo da perda | 56594 | 5839 / 39601 | zero índices; firstIndex zero; uma instância |

O fallback CPU já ocorria no extremo não zero. A capacidade declarada do
index buffer, 22.020.096 índices de 32 bits, supera o teto de 64 MiB do
caminho GPU; essa é uma condição suficiente para o fallback no código.
Não tratar o fallback em si como defeito.

Em F, `DrawIndirectMulti` lê os argumentos pelo ponteiro guest. Porém a
leitura pode disparar `HandleFault → BufferCache::ReadMemory`; a ausência
de uma chamada explícita de coerência não prova bytes antigos. O ZIP não
contém os 60 bytes desse batch nem sua versão nativa/produtor no instante da
leitura. O f0c140 capturado escreve outro recurso, `0x20934d2a0`, e não pode
ser usado como produtor desses registros.

A próxima comparação fica restrita a **versão do registro → bytes
consumidos → argumentos do comando**, com último não zero e primeiro zero
preservados juntos. Se os bytes consumidos divergirem da versão comprovada,
fixar a publicação/leitura. Se coincidirem, retirar essa leitura do conjunto
de operações divergentes e seguir o produtor registrado. Capturar também
as transferências e escritores observados permite continuar offline; um
descritor que cobre o endereço ou um primeiro fault de página continua sendo
candidato, não prova de último store.

O intervalo global G0/P8 continua sem certificado integral. Esse estado
não veta a aquisição da aresta concreta acima e não reabre os certificados
de posição, máscara, count-transfer, BC1 ou stencil já obtidos.

## Encerramento da sessão

Depois do frame 490 houve um `Fatal Error` no guard de `ComputeTextureDesc`
(`descriptors.cpp`), com saída 65. O guard e o caminho de saída são idênticos
à #744. A captura causal já estava fechada no frame 207. Os valores do
descritor que acionou o guard não foram registrados; portanto não há prova
de regressão da #745 nem fundamento para transformar esse guard em uma
correção de iluminação.

Não há correção visual comprovada nesta captura. Há uma eliminação
reproduzível de 45 cálculos de posição e uma fronteira de origem de comandos
precisa, que substitui sondas repetidas dos mesmos vértices.
