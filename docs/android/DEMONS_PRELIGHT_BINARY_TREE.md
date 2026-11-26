# Demon's Souls PPSA01341 — árvore operacional V2: bisseção causal
**Normativa a partir de #738.** O histórico V1 abaixo é evidência, não autorização
para escolher outro ramo. A árvore V1 misturou testemunhas W0/W1, expandiu folhas
sem estabelecer causalidade global e não resolveu a raiz. Não repetir isso.

## 0. Fixar o caso e os extremos ANTES de descer
ZIP #737 20261126-102508, owner 0x287ae0000, layer0 mip0, 1600x900.
Consumer q33 f311 submit8741 seq2729 version10716: 1440000 RGB não zero.
Consumer q0 f312 submit8782 seq2730 version10900: 651 RGB não zero;
q33 seq2732 confirma 651. f313 q33 seq3080: 672.
É a primeira queda observada depois da janela de população plena f231–311,
não uma prova de que todos esses pixels deveriam manter a cor entre frames.
O frame 312 tem 45 guest_command, 38 draw_prepared, 38 draw_execute,
mas nenhum draw_before/draw_after nessa captura. O journal termina com
1792 eventos perdidos, 26 full samples perdidos; a fronteira não foi
reconstituída. F348 já é posterior (zero), e W1 volta a produzir cobertura
em f503 embora o problema global persista. W0 e W1 não qualificam como
testemunha de causa global; manter seus resultados locais no histórico.

G0 (pré-condição, não ramo causal): fixar uma amostra/primitive/objeto que
contribui para a falha, versão da imagem, draw/ciclo guest, índices,
instância, câmera, estado e inputs. Obter saída esperada para os mesmos
dados via contrato guest/replay independente. Se a testemunha não estiver
demonstrada, permanecer em G0; não selecionar outro nó por associação.
A transição f311→312 é a âncora temporal para adquirir essa testemunha,
não o oráculo de cor. Frames absolutos mudam por instrumentação. Usar
mesmo save/configuração e evento guest de transição, registrar intervalo
e dados reais; se não reproduzir, não comparar cenas diferentes.
P06 e demais podas V1 valem apenas para as operações e inputs certificados.
Se a expectativa de albedo falhar e o defeito estiver depois do G-buffer,
G0 não autoriza declarar pré-iluminação provada. Uma nova árvore do domínio
correto exige demonstrar essa fronteira, não seguir pistas alternativas.

## 1. Espaço de busca fixo e predicado cumulativo
C0 entrada/contrato certificado (PASS).
C1 emissão/execução guest: visibilidade, comandos, argumentos, instâncias.
C2 fetch/loads: índice, vértices, descritores e dados, produtores e versões.
C3 saída VS/estágios geométricos: cálculo e export XYZW/clip.
C4 montagem, clipping, viewport, culling, raster: cobertura esperada.
C5 testes de fragmento: discard, depth, stencil, coverage e seus operandos.
C6 valor de export material: ISA/PS e dados efetivamente lidos.
C7 valor gravado: MRT, máscara, formato, blend, clear, store e escritores.
C8 publicação/consumo: versão/alias/layout/barreira, bytes entregues à luz (FAIL).

Cada classe cobre a primeira falha do contrato daquela fronteira para
a mesma testemunha. Dependências fora da linha (ex.: stored depth,
texturas) pertencem ao contrato da etapa que as consome; se a classe
for selecionada, construir DAG de produtores, ordenar topologicamente
as operações causais e bissectar o ancestral certificado da falha.
Não confundir um valor final coincidente com prefixo correto: P(k)=
todas as fronteiras C1..Ck relevantes certificadas com dados reais.
P(k) é cumulativo e monotônico; testar apenas saída Ck pode esconder
compensações, overwrite ou entradas erradas. Um mismatch upstream
suficiente certifica FAIL; ausência de captura é BLOCKED, não FAIL.
A prova de prefixo pode exigir replay e verificação de várias fronteiras,
não prometer um único contador ou uma build por decisão.

Estado imutável por testemunha: lo=último prefixo PASS, hi=primeiro prefixo
FAIL certificado; iniciar [0,8] somente com G0 e C8 demonstrados.
m=floor((lo+hi)/2). P(m)=PASS -> lo=m; P(m)=FAIL -> hi=m.
P(m)=BLOCKED -> manter [lo,hi] e adquirir exatamente as entradas faltantes.
Quando hi-lo=1, a classe hi está isolada; não abrir irmão. Três decisões
suficientes para oito classes SE os predicados estiverem disponíveis;
não é garantia de três compilações nem de uma causa única global.
Se o primeiro erro não explica a falha final, não fabricar causalidade:
testar regressão da mesma cadeia, manter a testemunha e registrar que
o erro isolado não explica o sintoma; não reabrir irmãos por suspeita.

```
R [0,8] P(4): prefixo até cobertura raster correto?
  N A [0,4] P(2): prefixo até fetch correto?
    N A1 [0,2] P(1): emissão correta?
      N C1 emissão / S C2 fetch
    S A2 [2,4] P(3): saída VS correta?
      N C3 VS / S C4 raster
  S B [4,8] P(6): prefixo até export material correto?
    N B1 [4,6] P(5): testes corretos?
      N C5 testes / S C6 PS
    S B2 [6,8] P(7): valor gravado correto?
      N C7 store / S C8 publicação
```

## 2. Descida obrigatória após isolamento da classe
Dividir o conjunto finito de operações relevantes da classe em duas
metades por custo/evidência, ou número de operações quando equivalente.
Fixar o mesmo input/estado e a ordem causal; certificar extremos e o
prefixo intermediário. Ex.: classe C3: 64 operações relevantes no slice,
intervalo [0,64], testar prefixo 32, depois 16 ou 48, etc. Se a falha
for um load, o slice inclui endereço, bytes, versão e produtor; não
trocar para uma malha de outro frame. Se várias entradas são necessárias,
verificar prefixo topológico, não supor independência nem seguir um
endereço por mero overlap. Repetir até uma operação concreta com
entrada, semântica guest, resultado esperado e observado. Corrigir
e verificar a mesma testemunha, e depois a falha global. Sem fix por
NaN, alteração de constantes, luz artificial ou inferência de câmera.
Não reabrir grupo eliminado salvo nova evidência contraditória,
registrando exatamente qual certificado foi invalidado.

## 3. Próximo passo e controle de custo
ESTADO ATUAL: G0 BLOQUEADO; [0,8] apenas espaço potencial. N00 antigo não
resolvido. Nenhuma classe global eliminada por W0/W1. #738 já disparada
antes da revisão; pode fornecer evidência local, não define o próximo
nó. Esta revisão é docs-only, sem nova build automática.
Próxima aquisição, se necessária: ancorar no primeiro evento de queda
do consumer layer0 depois de população plena, em vez de fase tardia.
Manter buffer circular compacto de eventos reais (clear/load/store,
draw/dispatch, cópia/publicação e versão); preservar a janela anterior
à queda e o ciclo da queda, com snapshots de amostras e dados suficientes
para estabelecer G0 e avaliar P(4) por replay. Capturar operandos de
fronteiras dependentes na mesma execução quando disponíveis. O gatilho
por população serve apenas para retenção, não prova de erro. Não usar
budget consumido por logs W0/W1. Verificar completude e export antes
de interpretar; se não houver referência, explicitar bloqueio e não
criar build incremental por suspeita.
Ledger obrigatório por decisão: ID testemunha, versão/recurso,
intervalo antes, mid, PASS/FAIL/BLOCKED, prova e arquivo, intervalo
depois, subárvore podada, custo e lacunas. Somente um intervalo ativo
para essa testemunha. Nenhuma mudança de ramo por curiosidade.

---

# Histórico V1 (somente evidência; não usar como roteiro operacional)
# Demon's Souls PPSA01341 — árvore binária causal pré-iluminação
Versão 1.0, etapa 1: construção e poda por evidência. Não é uma correção
de renderização e não autoriza uma build por hipótese. A árvore governa
a próxima investigação, substituindo a escolha livre de suspeitos.
Fontes: DEMONS_CAUSAL_MATRIX.md, DEMONS_PC_RENDER_PARITY_2026_11_25.md,
ZIPs F 222007, 223802, 224726, 230251, 231956, 233709 (builds #720–725).
Revisões/identidades dos ZIPs constam na matriz. Os dados foram
reconferidos diretamente nos arquivos de albedo e geometria #721–725.

## Raiz concreta e limites
#721 223802 fase 0 f190–191: 1.440.000 palavras de albedo não zero em cada
um dos quatro registros. Fase 1 f286–287: 41 draws de material, todos
com zero amostras aprovadas, albedo zero antes/depois de todos, nenhuma
palavra alterada; os 42 registros originais de escrita de depth sem layer0
no attachment observado também têm zero amostras aprovadas (41 draws
de cenário mais um registro adicional). Fases 2/3: 475 perdas de palavras
não zero, atribuídas a 552d823d, depois da fase de início; não são causa
demonstrada do início da perda.
#725 233709 fase 0 f211–212: 1.440.000 não zero. Fase 1 f283–284:
1.154 -> 1.252, 42 draws, 329 ganhos e 231 perdas, zero discrepância
de contagem entre snapshots consecutivos. Fase 2 f331–332:
88.328 -> 88.518, 46 draws, 190 ganhos, zero perdas. Fase 3 f474:
42.699 -> 42.853, 23 draws, 631 ganhos, 477 perdas. 552d823d f474
passa 1.397.147 amostras, mas modifica a contagem não zero de 43.330
para 42.853. Cobertura não é sinônimo de albedo válido. As fases não
constituem captura contínua nem mesma câmera. Não se certificou uma
palavra errada versus uma referência guest para um pixel em particular.
A ausência de cor antecede a iluminação; a primeira divergência causal
ainda não foi localizada. A árvore inclui tanto produção ausente quanto
perda/publicação posterior; nenhuma dessas classes é presumida verdadeira.

## Poda de operações (não podar seus operandos nem generalizar)
P01: BC1 0x208a30000, #725 12 x 7 mips = 84 payloads idênticos a guest
linearizado segundo o layout do projeto; excluir transporte/detiling
dessa imagem. Não excluir cálculo de UV/LOD, sampler, compare ou origem
de constantes. O oráculo de layout não é uma captura do PS5 real.
P02: stencil #724 e #725, nos draws medidos stencil-only = survive;
excluir stencil como explicação desses draws. Fora desses draws, só
reabrir se teste isolado mostrar rejeição não explicada.
P03: comparação reverse-Z, #725 25 conjuntos less/equal/greater
particionam exatamente sobreviventes, igualdade zero; CPU raster/depth
nos dois pares menores: f283 4/4 e 72/25, f474 107/0 vs GPU108/0
e 1674/0. Excluir comparador/equal desses casos. Manter origem da
profundidade incoming e stored, e raster/transform para outros draws.
P04: cb3b 204/204 arquivos (184 únicos), f0c140 1344/1344 transferências;
excluir essas aritméticas/transferências verificadas. Não excluir
produção dos operandos de visibilidade, comandos/argumentos ausentes,
outros dispatches ou loads não verificados. O primeiro gate fb18
reproduz a decisão para os operandos medidos; não certifica o produtor
do valor nem todos os objetos/estágios.
P05: seis pares menores same-frame #725, posições/clip iguais;
excluir diferença depth/material de posição nesses pares, não a
correção absoluta dos operandos guest nem todos os VS.
P06: iluminação, transporte scatter 62375 linhas, máscaras/luz,
atlas, relight, HDR, exposição/pós-processamento ficam fora da árvore.
As equivalências PC já integradas (PS activation, MRT, HTile, DPP,
DMA alias) não são prova de todo caso dinâmico. Modos B1/C1/C2/D2
negativos não certificam todos os caminhos; não repetir o sweep.
Não reabrir P01–P06 sem registrar ID, nova evidência incompatível e
motivo da mudança de escopo.

## Árvore de decisões (S/N)
Cada nó interno tem exatamente duas arestas. Sem dados suficientes,
o nó fica BLOQUEADO (não seguir nenhuma aresta, não inventar terceira
resposta). O valor esperado vem do comando/ISA guest e replay
independente com operandos reais; PC com os mesmos dados pode servir
como verificação adicional, não uma comparação entre cenas arbitrárias.
O predicado é avaliado por pixel/amostra/triângulo/recurso identificado;
a mesma execução pode ter testemunhas em ramos diferentes. A árvore
não presume uma única causa nem que todos os pixels não zero sejam válidos.

N00 — O valor de albedo esperado chegou a existir na subresource
correta, no ciclo de renderização em que deveria ser produzido?
├─ S → N10 — O primeiro ponto de perda ocorre em um draw que escreve cor?
│  ├─ S → N11 — A saída do pixel shader já diverge do valor esperado?
│  │  ├─ S → L01: instrução/operando do shader que sobrescreve albedo.
│  │  └─ N → L02: export MRT, formato/swizzle, máscara, blend, store.
│  └─ N → N12 — O conteúdo da subresource foi realmente alterado?
│     ├─ S → L03: clear/load/store, copy, resolve, DMA, escrita compute,
│     │            alias ou ordem do último escritor.
│     └─ N → L04: seleção/publicação/consumo de versão, owner, layer,
│                 mip, view, descriptor, barreira ou sincronização.
└─ N → N20 — O draw/dispatch esperado foi emitido e executado
              com índices, instâncias e estado corretos?
   ├─ N → N21 — O comando guest original contém a operação correta?
   │  ├─ S → L05: tradução PM4, indirect, offsets/counts, submissão,
   │  │            predicação, execução/ordenação (P04 já podado).
   │  └─ N → N22 — A entrada do produtor guest de visibilidade/emissão
   │               é correta conforme seu próprio contrato?
   │     ├─ S → L06: primeira operação guest de seleção/culling,
   │     │            shader compute/CPU, argumentos ou predicação
   │     │            divergente (P04 já podado).
   │     └─ N → L07: produtor dos dados de visibilidade, streaming,
   │                 câmera, transforms, estado CPU/FEX/HLE ou asset.
   └─ S → N30 — As primitivas esperadas chegam à cobertura raster?
      ├─ N → N31 — A posição/clip exportada e topologia/índices
      │             coincidem com o cálculo guest independente?
      │  ├─ N → N32 — Os operandos efetivamente lidos são os esperados?
      │  │  ├─ S → L08: primeira instrução de VS/mesh/geometry/tessellation,
      │  │  │            conversão de formato, transform ou export divergente.
      │  │  └─ N → L09: index/vertex fetch, SRT/V#, BDA, constantes,
      │  │               cache/alias/versão ou produtor de buffers.
      │  └─ S → L10: montagem, clipping, viewport/scissor, winding,
      │               culling, raster state, amostragem ou escala.
      └─ S → N40 — Os fragmentos esperados sobrevivem aos testes do PS?
         ├─ N → N41 — As entradas e valores amostrados do predicado
         │             são os esperados?
         │  ├─ S → L11: primeira comparação, EXEC, discard,
         │  │            interpolação ou controle de fluxo divergente.
         │  └─ N → L12: coordenadas, interpolantes, sampler, UV/LOD,
         │               descritores/constantes, origem dos operandos
         │               (transporte da BC1 P01 excluído).
         └─ S → N50 — Os fragmentos esperados passam pelos testes
                      de profundidade/coverage restantes?
            ├─ N → N51 — O incoming depth/coverage corresponde
            │             ao replay guest de posição/export?
            │  ├─ N → L13: produtor do incoming depth, export Z,
            │  │            interpolação, viewport/clip/bias.
            │  └─ S → N52 — O stored depth/estado corresponde ao
            │               último escritor esperado para a amostra?
            │     ├─ N → L14: primeiro escritor de depth divergente,
            │     │            clear HTile, owner, alias, cópia, ordem.
            │     └─ S → L15: se teste efetivo difere do replay, estado
            │                 residual não certificado; se ambos
            │                 rejeitam, rejeição legítima, descartar
            │                 testemunha e seguir outra expectativa
            │                 comprovada. P02/P03 não reabrir por contagem.
            └─ S → N60 — O valor exportado pelo material é correto
                         segundo o ISA com as mesmas entradas?
               ├─ N → N61 — Entradas efetivamente usadas são corretas?
               │  ├─ S → L16: primeira instrução de material divergente.
               │  └─ N → L17: texturas de material, atributos, loads,
               │               descritores, sampler, constantes e produtor.
               └─ S → L18: export/slot de MRT, máscara, formato,
                            blend/store, owner/subresource do G-buffer.

L15 é um terminal de invalidação de testemunha, não uma causa presumida.
Em L01–L18 uma classe residual não é uma conclusão: identificar a
instrução/função/transferência concreta e seu produtor. Se a operação
corresponder ao contrato para os operandos reais, rastrear o produtor do
operando errado, mantendo a identidade. Se os operandos e a saída
corresponderem integralmente, a expectativa da testemunha não está
demonstrada: fechar sem fix. Se múltiplos operandos divergem, manter
ambos os ramos ativos e ordenar pelo primeiro produtor/tick.
A ordem do hardware (early/late tests) pode variar; N40/N50 representam
testes contrafactuais readonly sobre as mesmas amostras, não cronologia
obrigatória do hardware. Não alterar semântica da renderização original.

## Estado inicial (antes de #726)
N00 BLOQUEADO para a transição inteira: #721 sustenta prioridade N20
na janela f286–287, mas não elimina N10–N12 no intervalo entre f191 e
f286. #725 sustenta também perda de material/cobertura; números de
amostras não provam albedo correto. N20/N30/N40/N50 parcialmente
observados, não concluídos para todo o cenário. N31–32 e N51–52
bloqueados para W0/W1. N60–61 permanecem abertos. Nenhuma folha de
causa raiz certificada. #726 (baa4bff) já disparada antes desta árvore:
só utilizar se fornecer evidência que resolva um nó, sem mudar a árvore
por ausência de captura. Não iniciar nova build na etapa 1.

## Evidência mínima por nó e ordem operacional futura
N00/N10/N12: histórico por mesma imagem owner/version/mip/layer,
pixels-alvo e referência esperada, snapshots antes/depois de todos os
tipos de operação, begin/end de render pass, clear/copy/dispatch
e versão efetivamente lida antes da luz. Delimitar último ponto correto
e primeiro ponto incorreto; investigar a fronteira por bisseção
temporal em uma reprodução estável (não bissectar contagem de cor
não monotônica entre câmeras). Se não houver replay estável, usar
histórico contínuo compacto + janela de detalhes pré/pós evento,
sem confundir ausência de captura com ausência do evento.
N20/N21/N22: inventário dos comandos guest e draws executados, IDs de
draw/submit, conteúdo de argumentos e índices, estados de predicação,
identidade de objeto/instância, bytes dos produtores dos argumentos.
N30/N31/N32: mesmos índices/triângulos, posições XYZW/clip, viewport,
estado raster, operandos SSA reais, endereços de loads e bytes; raster
independente com as regras do guest. Não basta VS depth=VS material.
N40/N41: replay de cobertura sem/ com discard preservando os demais
dados; operandos efetivos de comparação, UV/LOD, interpolantes,
sampler e imagem, máscara EXEC; não recapturar BC1 já verificada.
N50/N51/N52: amostras idênticas, depth incoming, stored antes/depois,
clear, compare/bias, produtor por pixel, version/tick; replay readonly.
N11/N60/N61: valores exportados pelo PS (incluindo zero válido),
mesmos materiais e recursos, registro de destino e valor final
esperado após máscara/formato/blend. Compare em representação real,
não apenas contagem de palavras não zero.

Casos W0 (0x27c3a7280/12765), W1 (0x27d9906c0/57066), f474 s14134/14137
do #725: usar como testemunhas de produção já localizadas apenas
se o mesmo objeto/pixel for esperado pelo contrato; não presumir W0
visível, nem que W1 deveria passar todos os fragmentos. Incluir
testemunha do início da perda do albedo (prioridade N00) e controle
positivo de material com saída válida, não somente malhas tardias.
Reutilizar ZIPs e replays existentes antes de decidir captura nova.
Selecionar próxima divisão que separa mais causas ativas com
evidência no mesmo ciclo; capturar em uma execução as entradas de
ambas as saídas e os próximos nós dependentes. Sem build por ramo.
Registrar a cada avanço: nó, testemunha, resultado S/N, prova,
ramos eliminados, próxima divisão, operação/versão/bytes e arquivos.
Não usar porcentagem de hipóteses eliminadas como prova de causa.

## Critério de conclusão
Uma folha só vira correção quando se comprovar a primeira divergência
entre resultado esperado e observado com entrada real, causalidade
até albedo, e teste de regressão com a mesma testemunha. A correção
deve restaurar a saída prevista sem forçar cor, depth, contagens ou
luz. A árvore é a etapa 1; não é pedido de novo APK por si só.


## Etapa 2: implementação N00 no modo F
Histórico contínuo a partir do primeiro uso da faixa conhecida, não do
trigger de HDR: draw preparado/executado/omitido e amostras de 64 palavras
antes/depois de cada draw layer0, identificadores de objeto/backing/versão,
load clear e máscara, depth e comandos de prepass; leitores reais das
filas 0/8/33 antes de dispatch direto/indireto, full digest/população uma
vez por fila/frame e grid, storage bindings antes/depois; journal de
registro, upload, clear, alias/invalidação, copy e claims de cache. Claims
são registros de intenção/ownership, não prova de escrita GPU. Eventos
por sequência, submit/tick, com 12 chunks de 2 MiB, orçamento de amostras
e status explícito de truncamento. Grid não prova ausência fora das 64
coordenadas. Readbacks são síncronos e podem perturbar timing. Não há
oráculo guest completo; N00 permanece bloqueado quando falta palavra
esperada ou histórico completo. Teste unitário garante que o classificador
não decide apenas com zero/contagem, lacuna ou identidade distinta.
Retirar recaptura de BC1, replay antigo de depth/stencil e coleta de
vértices direcionada no modo F enquanto N00 estiver aberto; manter demais
mecanismos e modo E. Prioridade de exportação dos chunks no ZIP.


## #727 — primeira fronteira observada, ZIP 20261126-075253
Revision 174725d, mode F. 56,386 consecutive records (no internal gap),
frames 1–293, 709 full consumer snapshots across 247 frames. Journal
filled: 48,128 dropped events, 700 sample drops, 699 full drops.
First sharp collapse f238 1,440,000 -> f239 658 nonzero RGB words.
f239 s6723 draw33122 VS c96f3fcde8141266 PS 552d823dcfcf2dee,
27 indices at 0x20fb00000, after 21 preceding material draws leave
all 64 sampled words unchanged. Before33123 all colored; after33129
all 0xff000000. No load clear, same native object, version11334->11342,
depth compare EQUAL (2), no depth writes. q0/q8/q33 read identical full
digest/grid. No upload or invalidation between these observations.
Observed N10=draw for physical overwrite; N00 causal expectation
still blocked: colored before-image could be previous-cycle data,
PS552d may intentionally produce black, background prepass could
legitimately own pixels. Do not disable background.
f239 prepass VS aff9c7ef/PS651f53 27 indices seq32948.
Second collapse f264 1,440,000 -> f265 13,264 -> f266..269 zero.
f265 s7716 first layer0 draw PS afd9334e, VS13f07ce8, 22236 indices,
0x27cbfd730, before43144 colored64, after43152 all zero. Four GPU
invalidations seq43136..43139 at 0x287ae0000,0x288160000,0x2887e0000,
0x288e60000, length 0x680000 each, cause GpuBuffer(5); upload seq43145–46
BETWEEN before snapshot and draw execution. Cannot attribute loss
to afd933. Repeats f266–268, twice f269. Distinguish DMA fill/copy/BDA
origin and expected bytes from refreshed image before draw; legitimate
clear is not corruption. Record exact buffer producer and route,
draw_acquired after FindRenderTarget refresh before attachment transition
and draw, skip pending folded clear. Record background SRT and depth
before/after matched prepass, native module names. Reduce duplicate
claims, unrelated sampled compute, depth-only after records except
background, expand journal 32MiB; full snapshots q0/q33, grids q8.
No production graphics changes or proof of causal first divergence.


## #729 — N00 physical split, ZIP 20261126-081531
67,299 consecutive events, 24,152 prepared and 24,152 matched execute;
358 frames (46..403) with 690 full consumer observations, 332 double
reads and no differing digest within a host frame. Journal truncation
28,028 events; no claim of continuous late gameplay. First collapse
f211 (second cycle) -> f212 consumer 655 words, draw15574 PS552d 27
indices, unchanged pre-acquire 64/64 colored, afterward 0/64; matched
prepass15484 same 108 index bytes/addr 0x20fb00000, 63/64 stored depth
zero before, all 64 nonzero after, exact same 64 depths at material.
This background operation writes depth where prior scene geometry did
not; no evidence of incorrect compare or export. Do not disable it.
Second collapse f247 1,359,577 -> f248 11,301 -> f249/250 zero.
381 clear_image_candidate commands, all value 0 size0x680000;
1 CPU dma_fill zero. f248 seq23820..27 four layer clear candidates and
invalidations; f248 draw23830 before61/64, acquired23834 all 64 EXACT
zero, after all 64 zero. All eight dirty acquisitions exact zero
before original draw and unchanged after that draw. This is consistent
with requested zero fill, not corruption; only sampled words certified.
After this clear, every material draw has corresponding prepass by
index address, count, byte length in current frame: f249 11/11, f250
7/7, including both indirect records (actual executed args unknown).
Persistent W0 12765 indices, addr0x27466f2a0; W1 57066 indices,
addr0x27b7c3a00, both PS31e6/e31a and VS8685/13f0, present f248-250.
Addresses are session-specific. No evidence any prepared command
was omitted; pre-prepare guest admission not recorded previously.
N00 -> N20 is the operational path for post-clear cycles: sampled
retention/publication and upload match observed command. Still no
independent proof of expected visible geometry or absence outside
sampled pixels. N20 split requires raw guest admission and native
indirect args, N30 split needs original coverage/clip for same draws.
Next F: phase windows triggered by full albedo population collapse
(50%+ to <=10%, twice, NOT a causal conclusion), plus late adaptive
window. Reduce prior root census to these windows. Record guest-facing
command and return before preparation, link original draw/query,
read actual indirect input and fixup output (bounded 4KiB) and count,
producer range/tick. Original precise occlusion queries for all
relevant draws in two host frames (4096 capacity), no old replay
variants. Paired exact native index streams/position/descriptor
captures only W0/W1 by count and same address/instance, no assumption
that W0 is expected visible. Prioritize query and vertex exports ahead
of capped 16MiB journal. Reuse existing readback instrumentation;
readbacks may perturb timing. No production semantic change.


## #730 — N20 validation and concrete N32 input witness (083233)
5456 consecutive journal records, no dropped events or samples. 610 guest
commands, 547 prepared returns (540 selected prepared/execute pairs),
49 CPU fallback, 14 zero count. 134/134 native indirect records match
independent mode-0 fixup replay (77 indexed, 19 nonempty). Do not infer
all guest culling correct or all fallback commands absent. Coverage phase2
f251–252: all ten scene prepasses and ten material draws pass zero; phase1
f220–221 W0 12765 indices has 86/30 passes; late f429–430 W0 has zero,
W1 57066 indices has 25675/12453. Phase3 paired native vertex captures:
W1 37358/37358 identical finite XYZW/clip, W0 2624/2624 identical but
all four XYZW are NaN, clip distance zero. W0 index stream identical,
all vertices referenced. Prepass slot2 XYZ 0x20cca0e40 31488 bytes
7872/7872 NaNs: 5145 0xffc00000, 2727 0x7fc00000. Material slot3
same address/content; slot4 other XYZ 0x20ccab240 also NaN.
Packed slot0 0x20cca8940 2621 x e0080200, 3 x 20080200. GPU-dirty
CPU backing zero is NOT evidence of corruption. W0 transform buffer
contains finite data; no independent transform operand mapping for
this VS variant. N32/L09: nonfinite values already present before VS;
compare producer output vs actual consumer. Intentional NaN cull remains
possible; no guest oracle demonstrating that W0 should be visible at
f429. W1 finite control, other scene meshes remain open. No fix yet.
Next F uses dynamic live W0 range discovery, earlier session addresses
only as seeds, bounded prior writer history and exact consumer words.
For overlapping compute descriptor writers or learned writer hash,
capture actual input descriptors/SRT/bytes before dispatch and tracked
range words before/after, native module name and guest code. BDA or DMA
writer candidates reported, not proof of a write. Peek existing buffer
only (no allocation). Upstream inputs larger than budget marked partial.
Share capture gate across queue threads; #730 phase1/2 query records
had command ID 0; thread-local population gate was a potential cause.
Retain bounded phase2 position pair. Prioritize producer files and exact
modules in ZIP. No semantic change.

Binary extension:
N32 observed nonfinite input -> N70: does exact last producer already
write NaN? S -> N71: guest ISA with actual inputs should write NaN?
S -> legitimate cull or upstream invalid operand, trace input producer;
N -> first differing compute instruction, fix with regression.
N -> N72: same bytes at producer and consumer? N -> first overlapping
writer/transfer/version; S -> invalid witness, check exact identity.
Unproven producer or missing actual operand -> BLOCKED. Do not treat
NaN alone as proof of error. Keep N30/N40/N50/N60 active for finite W1.


## #731 — first XYZ transition bounded (ZIP 20261126-085152)
W0 12765 indices: exact live XYZ range 0x20a231510 (31488 bytes)
has 0/7872 NaN at f195 tick11690 and f196 ticks11719..11835,
7872/7872 at f226 tick14733, unchanged through f270 tick20222.
Same range, not just same index count. f270 prepass and material
consume identical invalid XYZ; backing has only 4 NaN words and is
GPU-dirty (not an independent oracle). Other live ranges rotate:
0x3c264a600 finite f232, all NaN f269; 0x3c2640200 all NaN f265.
The observed executions of 22e0328668b00aa3 and d2bbd6fff133ed4e
did not change tracked XYZ; no producer transition in 12 captures.
Writer history empty for the invalid XYZ range; do NOT interpret
missing provenance as no GPU writer. Journal dropped 16896 events,
216 full samples and 324 samples. The phase/hash gate captures only
one execution per shader/phase and consumer gate waits 32 frames.
N70 remains BLOCKED: producer vs transfer not decided. N71 remains
BLOCKED: deliberate guest NaN culling not disproved. N00 causal link
to all albedo still unproven; retain finite W1 control and material path.
Next division: preserve a dedicated compact per-frame live consumer
and overlapping write journal independent of the saturated root
journal; compare exact XYZ ranges at every matched dispatch rather
than one phase sample. Preserve the first transition and its actual
input bindings. BDA/transfer claims remain candidates, not proof.
No production graphics semantic change or global illumination fix.


## #732 — N70 yes, N71 copy verified, upstream source split (091326)
Timeline 1388 consecutive records, 226 exact dispatches of 22e0328668b00aa3,
249 consumer records; no recorded drops. Nine transitions in two independently
allocated destination pairs: f242/243/244/245/246 and f265/266/267/268.
All 13 retained dispatches: slot0 control [7872,7872,1,0], groups123,
slot1 source 31488 bytes, slot2 destination 31488 bytes. Independent byte
comparison: all 102336 source words equal output, no mismatches. Example
f242 source 0x3a5700000 contains 3897 NaNs, dst 0x20d2c2f10 initially zero
NaNs, after identical to source. Both cycles first 1299 then 2482 vertices
have all three XYZ NaN. No evidence of arithmetic/transfer corruption in
this copy. N70 yes, N71 copy matches observed source, recurse N32 upstream.
N73: actual source guest backing vs actual GPU bytes and prior staging
upload/compute/BDA/DMA writer. If matching staging and backing, trace guest
CPU producer; if GPU-only, trace actual producer and input operands. Partial
or missing writer history is BLOCKED, not proof. Source addresses rotate;
learn from exact copy binding and retain prior aligned upload candidates
without assuming identity. Keep W1 and N00 albedo causal link open.


## #734 — N73 CPU source proven, N74 candidate producer
ZIP 092639, native revision 74965f2. 308 exact source observations f218+,
105 rotating addresses; 308 backing_ok and backing_equals_gpu, 308 GPU
not dirty, 308 latest full staging digest identical to source GPU digest,
307 latest transfer guest_backing_upload, no observed compute candidate.
11 finite, 2 x3897 NaNs, 2 x7446, 293 x7872. f223 finite, f224 3897,
f225 7446, f226 7872; f259..263 finite, f264 3897, f265 7446.
N73 yes: NaNs predate Vulkan upload, exact copy not causal error. N74:
identify actual CPU writer and guest operation; intentional NaN sentinel
cull is plausible, not proven. No FEX divergence demonstrated yet.
Timeline saturated (29446 saved, 11088 dropped); full copy reports evicted
by unrelated overlapping producer records. New build focuses exact copy
source; enables CPU write-fault history in mode F without ray history,
retains candidate MiB-aligned first 64KiB fault windows, exact guest RIP
and integer registers when available, code bytes read after fault,
file-read candidates, staging provenance, full source at each NaN
population transition. Dedicated bounded CPU journal and prioritized export.
First-write faults may not be the final NaN store; absent fault or code
is BLOCKED, not evidence of legitimate cull or FEX bug. W1 and N00 open.


## #735 — N74 CPU writer candidates, not final writer (094459)
Revision d7f39dc; 607 source records, no CPU journal drops, 167 distinct
addresses. 607 CPU/GPU equal, 597 have fault candidates, no code bytes
exported, no file-read candidate. NaN states 14 finite, 2 x3897, 2 x7446,
589 x7872. RIP 0x910003fb0 appears in exact range in 8/14 finite, 2/2
3897, 2/2 7446 and 580/589 all-NaN captures. f239 and f288 show RDI
page-aligned in destination and RSI progressing by 4096; tail RIP
0x910004069. This is consistent with copy, not proof of guest operation.
N74 remains blocked; do not blame FEX or intentional cull. Next F reads
actual instruction bytes and stack at fault using existing fault-safe
kernel-mediated ReadSelf (TryReadBacking yielded no code), captures
source page digest and NaN population before resumed store when RDI
matches fault address, compares same page with consumed destination.
If source already NaN and digest matches, trace earlier caller and source;
if not, distinguish later writes vs actual instruction divergence. Stack
words are candidates not verified return addresses. First-write faults
cannot certify complete stores. W1 and global N00 remain open.


## #736 — CPU AVX store and independent finite-geometry clipping (100418)
719 source records CPU/GPU equal, 9046 fault observations (may repeat event),
8592 with 32 code bytes, 5145 complete source pages and zero digest matches.
RIP 0x910003fb0 decoded C5 FA 7F 07 = vmovdqu [rdi],xmm0. 3307
observations, 1795 distinct fault IDs; 3258 RSI candidate pages already
have NaNs, 3305 stack[1] candidates 0x900bd453f. RSI is NOT the actual
store operand; digest mismatch alone cannot prove corruption. Capture
mapped XMM0, host store opcode, RSI prefix and eventual destination 16B;
read code near candidate caller. Missing mapping or later overwrites are
BLOCKED. Keep N74 scoped to W0, no speculative FEX correction.
Independent offline clip replay (same triangles and captured XYZW): W1
all 19022 triangles satisfy y<-w on all vertices in phase2 of #731 f270,
#732 f295, #735 f279, #736 f272. #736 W1 finite y/w [-1.6564,-1.2578];
W0 1299 nonfinite vertices, 2446 triangles with nonfinite vertices,
remaining 1809 triangles all y<-w; finite y/w [-1.4794,-1.3434]. Both
material and prepass match. f271 all nine material draws zero coverage;
f447 W1 13783/13844 material samples, W0 zero, so W0 alone cannot
account for all albedo loss. N30 clip compatible with actual XYZW; open
N31 projection/transform expectation, not proven wrong camera. #736
original prepass SPIR-V 7843cbd43b48179b: Y=asfloat(170)*asfloat(733)+
asfloat(739) for active path, W=asfloat(155)*asfloat(689)+asfloat(710),
Y accumulator 739 from 133*689+716, 716 from 135*650+677. Capture
these exact typed SSA IDs plus upstream operands at position export;
record prepass phases 1/2/3, preserve all clip positions and indices,
sample 1/64 projection operands separately to fit exporter. Compare
same identity and actual input constants; do not assume another frame
or count is identical. N00 remains open; no production semantic fix.


## #737 — N31 sampled arithmetic verified; transform provenance (102508)
297 CPU source records, all CPU/GPU equal; 1111 XMM0 observations,
1107 equal eventual 16-byte destination, four mismatches (not proven
store errors: later writes possible). 791 also equal RSI prefix; 316
match destination but not RSI. W0 CPU store deprioritized, not closed.
W1 f348 phase2 vs f503 phase3: exact same indices (57066), 37358 finite
vertices, same viewport; Y/W [-1.65826,-1.22331] vs [-0.84419,-0.32230],
0 vs 14320 changed depth pixels. All 584 sampled vertices per frame:
Y, W and both Y accumulators exactly reproduced from actual SSA. N31
arithmetic compatible with inputs; not proof camera/object expected.
Slot1 same 0x20f97e680 (160B), only words 7/19 change from -1.7599986 /
-0.2899988 to -514.87097/-514.87097. Slot0 same address, every word
changes; both slot0/1 GPU-dirty and backing differs. Slots3/4 identical;
slot2 448296 bytes not captured, different addresses. Cannot isolate
causal constant from two different scene states or assume erroneous
translation. W1 recovers coverage while global lighting remains wrong.
Next collect exact W1 constant consumer each frame, candidate compute
writer before/after full constant range, transfer journal independent
of saturated root journal, and full XYZ descriptor separate for phase
snapshots. Prior address seed is unconfirmed until live binding. No
semantic changes. N31 upstream provenance open, N00 still open.
