# Demon's Souls — comparação da leitura dos argumentos CPU

## Pergunta decidida pela próxima captura

A análise da [#745](DEMONS_APK_745_RESULTS.md) encerrou a comparação de posição
dos 45 draws com observação nativa: 294.320 componentes coincidem bit a bit.
O mesmo journal mostra o batch de cenário passar de 98.304 índices para zero
antes da perda de albedo. Faltam somente à **próxima comparação local** os
valores consumidos e a versão independente dos seus três registros de 20 bytes.
Isso não significa que todo o produtor anterior esteja certificado.

A pergunta é: **o processador de comandos leu e emitiu os argumentos da versão
correta desses registros?** Zero é um valor possível, sem expectativa artificial
de quantidade de geometria.

| Resultado da comparação | Redução causal |
| --- | --- |
| Leituras e argumentos emitidos coincidem com a referência independente da versão observada | Retirar leitura/decodificação dessa operação do conjunto de primeiras divergências. Seguir a origem dos bytes, usando os produtores e operandos retidos. |
| Leitura difere de bytes GPU autoritativos, com identidade e ordem estáveis, ou emissão difere do contrato com leituras íntegras | Fixar o contraexemplo na publicação/leitura ou na emissão indicada. Resolver a primeira operação divergente antes de investigar produção legítima de zero. |
| Mudança observada durante a comparação, referência ausente ou transporte incompleto | Informar exatamente registro, intervalo e motivo. Preservar as comparações válidas dos outros registros; não declarar PASS/FAIL global. |

O relatório é local à aresta. Não muda automaticamente o estado global G0/P8
e não usa esse estado como autorização para compilar.

## Seleção do caso e retenção do episódio

O endereço é aprendido do pacote efetivo, sem selecionar uma fonte apenas por
semelhança de shader. O formato observado na #745 é indexed, três registros,
stride 20, índices de 32 bits, capacidade 22.020.096 e vínculo ao depth/G-buffer
do caso. A fonte observada foi `0x21de00078`, com extremos 54823 e 55986;
o material zerado é 56594. Esses números identificam a evidência anterior,
não impõem os números de comandos da próxima execução.

Cada lote inclui endereço, frame, submit, tick, capacidade/base de índices,
depth/targets, motivo do fallback e token do comando pai registrado pelo renderer.
Os cinco DWORDs e usos repetidos são preservados nos acessos originais. O tuple
passado a `DrawIndex` fica separado para conferir a emissão.

O último lote não zero e o primeiro lote zero subsequente têm slots próprios.
Eles continuam móveis até o **trigger de perda de albedo já existente**: um novo
positivo substitui o candidato anterior e limpa o zero antigo. O trigger sela
o episódio, registrando `observed_loss` e o último comando observado por stream.
Assim, um zero legítimo durante o carregamento não fixa prematuramente o caso.
Os slots não dependem da janela dos 50 draws preparados.

## Ordem da aquisição e autoridade dos bytes

1. Observar estado do intervalo sem criar, sincronizar ou substituir seu buffer.
2. Preservar os valores dos acessos originais que alimentam o filho.
3. Congelar a referência daquele registro antes do próximo filho. O backing é
   lido por seu alias separado; para a GPU, enfileirar uma cópia do buffer nativo
   já existente, com handle, offset e estado de autoridade no enqueue.
4. Recolher as cópias GPU somente depois dos três consumos originais. Um buffer
   dedicado de 192 bytes impede que os readbacks normais dos draws reciclem o
   armazenamento da testemunha.
5. Exportar separadamente o estado no enqueue e depois da coleta. Uma publicação
   ocorrida durante a espera não altera retroativamente a versão congelada.

Não há `FindBuffer`, `ObtainBuffer` ou upload diagnóstico antes do consumo.
O modo com recording em outra thread é identificado explicitamente; a sonda não
introduz acesso concorrente ao scheduler para fingir uma referência válida.

O auditor deriva a autoridade **por byte** dos intervalos GPU-dirty e das
publicações pendentes. Flags de página CPU/GPU-modified são contexto, não um
substituto desse mapa. Bytes sem autoridade GPU usam o backing separado; um
espelho Vulkan antigo não produz um falso erro de leitura CPU.

Uma diferença CPU/backing em página já desbloqueada também pode resultar de
store guest concorrente fora dos hooks observados. Esse caso é `UNSTABLE`, com
a diferença preservada. Não se exige um rastreador universal de stores CPU
para aproveitar uma igualdade local ou uma divergência GPU comprovada.

Os hooks de fault/publicação usam slots fixos e atomics, sem alocação nem I/O
no handler. O epoch de mutação avança mesmo quando o ring de eventos lota;
eventos perdidos não podem fabricar uma versão estável.

## Dados para continuar no produtor na mesma execução

São observadas as escritas CP, `WriteBacking`, uploads, fills, cópias e publicações
que sobrepõem o alvo. Para compute, somente descritores de escrita sobrepostos
ativam a aquisição de alvo e operandos. O registro inclui:

- módulos guest, SPIR-V original e SPIR-V ligado da permutação usada;
- parâmetros do dispatch, grupos, wave/workgroup, UD, SRT e leituras escalares;
- descritores com ordinal/slot correto e payload dos buffers lidos ou atômicos;
- bytes do alvo antes/depois, identidade nativa, dirty/pending e ordem observada.

Um descritor sobreposto continua sendo candidato a escritor. Mudança antes/depois
não afirma por si só uma instrução de store ou ausência de outro escritor.
Imagem, GDS e BDA sem alvo comprovado têm lacunas explícitas. Não são lidos
recursos arbitrários para ocultar essas lacunas.

Os novos readbacks de compute usam a espera do scheduler que preserva os bindings
preparados; os callbacks de cleanup não são executados entre preparação e uso
dos handles. O handle do command buffer é obtido novamente após a aquisição.

## Orçamento e exportação verificável

O complemento CP tem limite de **8 MiB de metadados serializados** e **24 MiB de
blobs**, com até 8 MiB por geração. A prioridade é preservar os extremos e seus
operandos. Histórico móvel é descartado com contagem explícita quando necessário.
Os nomes imutáveis dos blobs pinados ficam separados do nome mutável do slot atual.

`ds_prelight_cp_args_manifest.json` relaciona lotes, eventos, gerações pinadas e
módulos externos. O exportador exige esse manifesto ao encontrar arquivos CP,
fecha as dependências antes dos opcionais e registra tamanho/SHA-256 reais no
inventário. O auditor recalcula integridade e FNV declarado; não aceita uma flag
do app como resultado da comparação.

As fixtures do exportador real exercitam 24 MiB de gerações junto a 54 MiB de
testemunho anterior, arquivos fora do ring, manifesto/blob ausente ou truncado,
e precedência dos obrigatórios sobre opcionais. A #745 continua com seus
375 arquivos obrigatórios completos; #741/#744 conservam as lacunas reais.

## Mecanismo de decisão antes do teste no aparelho

`tools/prelight_indirect_oracle.py` usa somente Python padrão. As fixtures
incluem igualdade com contagem zero, leitura alterada contra GPU autoritativa,
publicação pendente com dirty vazio, autoridade mista por byte, espelho GPU
antigo em página CPU-dirty, mudança temporal, tuple emitido incorreto, pai/range
incorretos, arquivos truncados e preservação de resultados válidos quando outra
parte da captura falha.

O teste C++ de política verifica seleção, retenção móvel, limites reais e a
barreira de três consumos antes da coleta. Ambos os testes entram no job que
precede a compilação Android. Os testes Java do exportador também executam na
construção do APK.

Validação local concluída antes do disparo da build:

| Mecanismo | Resultado |
| --- | --- |
| Oráculo indireto, incluindo mutações e transporte | 40 testes passaram |
| Política C++ de seleção, retenção, coleta, orçamento e digest | Compilou com `-Wall -Wextra -Werror`; todos os asserts passaram |
| Exportador Java efetivo | 15 fixtures / 79 assertions, mais 12 assertions sobre #741/#744/#745, passaram |
| Oráculo de posição atualizado com os opcodes da #745 | 34 testes passaram; os 45 draws reais coincidem bit a bit |
| Auditor indireto aplicado à #745 | 14 batches rederivados; 54823→55986; referência independente ausente explicitamente, sem fabricar um veredito |

Compilação nativa integrada e assinatura são verificadas no workflow Android;
o registro da entrega identifica o run, commit embarcado e APK exatos.

```bash
python3 docs/android/tools/prelight_indirect_oracle.py diagnostico.zip --output indirect.json
python3 tests/manual/prelight_indirect_oracle_test.py
g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/manual/demons_indirect_witness_test.cpp -o /tmp/ds-indirect-witness
/tmp/ds-indirect-witness
```

O teste no aparelho usa F, mantendo save, câmera, resolução e driver da #745.
A entrega técnica registra compilação, commit embarcado e assinatura estável.
Não há correção visual alegada antes de uma divergência causal demonstrada.
