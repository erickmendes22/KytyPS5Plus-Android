# Auditor da leitura de argumentos indiretos

`prelight_indirect_oracle.py` compara uma aresta local da mesma testemunha de
Demon's Souls: os valores usados pelo processador de comandos no fallback CPU
com a versão de memória observada por um caminho independente. A faixa inicial
é `0x21de00078..0x21de000b4`, três registros de 20 bytes. O script usa somente a
biblioteca padrão Python; não depende de LLVM, NumPy ou de uma captura de PC.

```bash
python3 docs/android/tools/prelight_indirect_oracle.py diagnostics.zip --output indirect-audit.json
python3 tests/manual/prelight_indirect_oracle_test.py
```

API: `audit_capture(path: str | Path) -> dict`. A CLI retorna 0 para MATCH,
3 quando existe uma DIVERGENCE local e 2 para INCOMPLETE/UNSTABLE. Esses códigos
descrevem as evidências; não são um gate de autorização de build.

## Interpretação dos resultados

- **MATCH:** os DWORDs preservados no ponto de uso coincidem com a referência
  independente e ordenada daquele registro. Contagem zero também pode resultar
  em MATCH. Isso não certifica o cálculo guest que produziu os argumentos.
- **DIVERGENCE:** pelo menos um DWORD consumido difere da referência da mesma
  versão, com autoridade, identidade e ordem suficientes. O relatório fornece
  o primeiro endereço, campo, bits usados e bits de referência. Também cobre
  emissão incorreta: leituras e referência coincidem, mas o tuple efetivamente
  enviado ao draw diverge da decodificação independente dos argumentos.
- **INCOMPLETE:** falta um operando, identidade, arquivo ou informação de
  autoridade/ordem indispensável ao registro indicado. Outros registros válidos
  continuam sendo comparados.
- **UNSTABLE:** há evidência de alteração ou ambiguidade temporal entre consumo
  e snapshot. Uma escrita posterior não se transforma em falha retroativa.

Nenhum resultado local declara P8 PASS/FAIL nem autoriza ou proíbe uma build.
O auditor não contém uma expectativa de quantidade de geometria ou cor.

## Independência da referência

Os valores `consumed` precisam ser preservados antes de uma leitura diagnóstica
que possa esperar a GPU ou publicar dados no backing. A referência GPU precisa
vir de um buffer nativo existente, com identidade, offset e ordenação capturados.
O esperado CPU vem do mapeamento separado do backing; não pode ser construído
copiando o array de valores já consumidos pelo processador de comandos.

Um espelho Vulkan antigo não é referência para uma faixa que a CPU alterou.
Da mesma forma, dirty GPU vazio não permite selecionar CPU se existe publicação
pendente da faixa. Propriedade ambígua ou uma referência temporalmente posterior
a uma sobrescrita é reportada, sem inventar um dono. Flags de sucesso do app
nunca substituem o cálculo independente da comparação.

O ledger `gpu_dirty_ranges` e os intervalos dos tokens de publicação escolhem
a autoridade **por byte**. Os flags `cpu_modified`/`gpu_modified` são de página
e não substituem esse ledger. Um mesmo DWORD pode combinar bytes GPU e CPU;
a referência resultante conserva a proveniência de cada byte. Uma diferença
pós-leitura em bytes CPU de página já dirty fica UNSTABLE: os hooks capturados
não excluem um store guest concorrente que não gere nova falta. Igualdade
continua produzindo MATCH, e a saída emitida continua tendo contrato calculável.

Os snapshots GPU são enfileirados depois dos loads originais de cada filho e
antes do filho seguinte. A coleta que espera a GPU ocorre somente depois dos
três consumidores, usando staging dedicado. O auditor usa `state_at_copy` e
o epoch do enqueue, não o estado posterior ao Finish, que pode completar uma
publicação legítima. A ordem de coleta é conferida contra as três observações.

Os arquivos obrigatórios são conferidos por tamanho e, quando declarado pelo
exportador, SHA-256 ou FNV1a64. O próprio auditor calcula e registra SHA-256 dos
bytes lidos. Dependências externas sem tamanho no manifesto nativo precisam
constar do inventário real `EXPORT-CLOSURE.json`. Uma
declaração `required_complete=true` não substitui a inspeção dos arquivos.

## Contrato conferido com o recorder C++

O parser foi revisto contra `graphicsRun.cpp`, `bufferCache.cpp`,
`demonsIndirectWitness.h` e a política de retenção efetivamente implementada.

| Emissor | Campos/associação usados pelo auditor |
| --- | --- |
| `DrawIndirectMulti` | `parent_command`, `args_guest`, count/stride, index base/limit/type, frame/submit/tick/stream e child/address de cada registro. |
| `Uses` e `UsesJson` | Cinco DWORDs brutos, todas as ocorrências de leitura, fases log/patch/index_address/count/instances/empty/draw, overflow/incompletude e tuple emitido. Base vertex conserva os bits uint32 e é interpretado como int32 na emissão. |
| `IndirectWitnessState` | Range20 exato, ledger GPU, tokens pendentes, identidade do buffer, generation/unregister epoch, propriedade de imagem e flags de página. |
| `QueueIndirectWitnessReference` | Referência `gpu_native` ou `cpu_backing`, backing por alias separado, native/offset/extent, estado e epoch congelados no enqueue. |
| `CollectIndirectWitnessReferences` | Payload nativo coletado após os três filhos, `collection_after_all_original_reads`, `observation_seq_collected`, tick final e estado posterior separado. |
| `SaveBatch` | JSON `kind=cp_batch`, schema1, seq próprio, cópia de eventos e arquivo pinado com os mesmos operands. |
| `FlushLocked` | `ds_prelight_cp_args_manifest.json`, `ds_prelight_cp_args_events.jsonl`, required_files com tamanho/FNV, slots por stream e perdas explícitas. |
| Retenção/`SealAtLoss` | Candidatos móveis last_nonzero/first_zero até `observed_loss{frame,queue,seq}`, `sealed` e `last_seen`; um batch já iniciado pode terminar depois do evento de perda. |
| Arquivos do produtor | `pinned_producer_files{file,source_name,bytes,fnv1a64}`. `source_name` identifica o nome mutável na hora de pinning; não se exige que sua versão atual ainda seja igual. |

Quando o journal de comandos guest ainda contém o pai/filhos, o auditor cruza
a identidade do pai e count/instances/index address efetivamente emitidos. Uma
contradição entre dois registros de observação é INCOMPLETE, não um defeito de
renderização inventado. Quando essa pré-história já foi expulsa, o lote pinado
mantém sua comparação local com essa ausência de vínculo explicitada.

`observed_loss` vincula a retenção à janela já selecionada pela investigação;
não é um oráculo de visibilidade esperada. Sua ausência não invalida um MATCH
local. Evicção da pré-história também não invalida endpoints pinados íntegros.
Drops e dependências ausentes ficam explícitos, preservando outros records.

Os 40 fixtures cobrem alteração do consumidor, emissão incorreta, GPU antiga em
página CPU dirty, publicação pendente, propriedade mista por byte, falta natural,
escrita posterior, coleta antecipada, identidades, hashes, truncamento, ausência,
drops, retenção e leitura equivalente em ZIP/diretório. O recorder completo não
foi executado num harness host: o header nlohmann não estava disponível no checkout
local. O contrato C++ foi conferido campo a campo; os fixtures exercitam o parser
e as decisões com o formato emitido, sem simular uma execução GPU real.

## O que a captura #745 já demonstra

O journal contém 14 pacotes CPU fallback para a mesma faixa. O último pacote
com contagem positiva é o comando **54823**, cujo filho0 usa **98.304** índices.
O primeiro pacote zerado é o **55986**. Nesse pacote e no material **56594**, os
três filhos usam contagem0 e uma instância. A flag `cpu_fallback` já existia nos
pacotes positivos; sua presença não identifica o defeito.

Não há nessa captura os cinco DWORDs brutos preservados em cada leitura nem
uma referência GPU/backing independente da mesma versão. Os eventos dos draws
filhos permitem recuperar alguns valores consumidos, mas não os dois últimos
DWORDs nem a origem dos bytes. O relatório histórico fica **INCOMPLETE** por
essas lacunas exatas. A ocorrência de uma falta de leitura natural pode publicar
dados no backing; o simples uso do ponteiro guest não prova incoerência.

Essa transição é derivada novamente dos eventos, sem aceitar o relatório da
investigação anterior como resposta esperada. Os certificados já obtidos dos
34 fixups GPU, dos stores cb3b, das posições e do raster permanecem no escopo
original. Não é necessário repetir essas comparações para adquirir esta aresta.
