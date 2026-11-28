# Demon's Souls — registro de eliminação causal por build

> **Diretriz vigente — confirmação explícita do usuário, 27/11/2026:** P8/P(8) foi descartado como modelo de investigação. O único método operacional é V4, por contratos causais locais. Não há obrigação de decidir uma divisão global nem reportar seu intervalo ou status. A documentação histórica abaixo conserva evidências e decisões anteriores, mas seus roteiros, prioridades e estados globais antigos não são instruções vigentes. Os identificadores P8_USE são apenas marcadores técnicos de captura.

## Estado operacional atual — captura #765

Contrato ativo: relight do mesmo probe espacial (4957 na captura #764, 3997 na #765). Captura original incompleta por seleção indevida por ID temporário. Sem novo corte causal. A #766 corrige a aquisição, preserva a identidade pelos dados reais e confronta o índice empacotado, a camada e os acessos ativos. [Relatório #766](DEMONS_APK_766_V4_RELIGHT_IDENTITY.md).

## #765 real / #766 — repair stable spatial identity and prove packed layer operation (27/11/2026)

- ZIP 215042 SHA256 5288e46c1d6fb0397e98cb2c5107f35b3ae171d9218bf80888616cbdcb892dfe, native git 7d8bc49, ZIP CRC PASS, 23/23 raw manifest files SHA/length verified. No ds_v4_relight files or selection events. Earlier q8/q33 capture at frame311 had zero groups; this is not the active contract.
- Same exact position [3246391296,3288334336,3246391296,1098907648] now has ID3997 in f718 and f742, previously 4957 f503/f527 in #764. All four traces match all 512 raw ray words bit-for-bit, SHA256 b1ca24a3f1866beb850bb37879533fa81fd3a7ead2021e382a21ba455fd292ca, FNV word hash a7b7333162e801bd. Native load trace matches the bound row in all four. 109/256 albedo nonzero, emission zero on 256, max RGB0.0001423957292, 765/768 output RGB words equal across sessions, 768/768 equal within each session. Actual mask records differ; don't compare whole dispatches as identical. The #765 filter silently returned when fixed numeric ID4957 was absent. This was our acquisition error, not a game rendering defect. No new causal contract closed.
- Legacy site12 still reports 76 nonintegral layers on 19 rays at both f718/f742 for this spatial witness, same census as #764. Other probes show similar values even during lit samples; legacy SPIR-V instrumentation cannot establish original EXEC. New native record captures original corner eligibility mask at 0x158c, actual buffer descriptor/index at 0x15bc, native read-only indexed dword at that binding, actual packed word/shift/mask at 0x16a4 and integer v40 immediately before original v_cvt_f32_u32 at 0x16e4. Independently check loaded dword, integer extraction and conversion; then actual shared layer at both 0x172c/0x1734. These are separate causal edges of the existing relight contract, not a new branch.
- #766 resolves position using every entry of actual dispatch map, and reads matching raw ray records. A unique exact position may have changed input (record it, not reject it). Multiple equal positions require a unique historical ray hash or explicit ambiguity. Uses actual probe ID and workgroup Z in header/record; host verifies map membership, raw load and identity. Reserve two dark captures and allow one earlier exact-row capture. Logs selection and periodic absence. New schema2, 17 sites, 598024 words / 2392096 bytes per trace, max 3 traces; retained exporter requires same files. No game output changes. 
- Local tests cover position renumbering, changed row, duplicate positions, unique exact-row selection, absence, wrong header/position/identity, integer shift mask and existing equality/mutation and footprint controls. Real legacy comparison above is offline and does not claim guest-active agreement. If native dword/extraction/conversion differs, isolate first proven mismatch and correct; if all match, use the actual active weighted samples and subsequent slices to continue the same producer contract.


## Registros históricos e evidências preservadas

## #761 real / #762 — indirect radiance q33, same pixel pair (27/11/2026)

- ZIP 20261127-201555 SHA256 1cbb6841b8029b53a87da6c68387bc08a18e90f3f56e458931f3e0baac54accd, core 1cc3209, 39/39 manifest entries verified. Writer shader 1ce1a858f62b5a11 frame353 submit10796 P8_USE_10 serial256096, mask guest 0x20ae6ae70 native11573980101893681772 offset175728, 351424 bytes. Reader frame354 submit10815 P8_USE_94, identical entire mask SHA256 59f339261f58ecc705ddcb9b94e41479e3a05a6774959228be0af1b3c9328b1a, no changed words, same native owner.
- Independently execute original producer ISA SHA256 6c2e72dfd79a528d48633244ce9387fa460fe27430c01632bb2643d53cca9f0e, exact scalar and four BEFORE buffers, 255 original wave64 workgroups over 34x19x17 grid. 10982/10982 masks (87856/87856 dwords) and 10982/10982 summaries bit exact. No injection of observed output. Both cluster generation and publication locally MATCH; do not reopen without evidence. Reproducible host adapter v4_cluster_isa.py uses captured dimensions and output census.
- Same q8 tile13700 lanes48/56 pixels (800,550)/(800,551), stencil86, zero incoming HDR. Corrected reference matches original cluster indices23792/23793, sample addresses and conditional RGB (max1 ULP), BRDF 474/474 within1 ULP. Only light0 contributes ~6.6e-5/~8.1e-5. Lights1/2 outside radius and light3 category rejected. Relative control, not healthy. Native samples still conditional inputs.
- Q0 excludes tile13700; q33 P8_USE_120 includes it (workgroup12196), but its HDR contribution is not representable at either witness: identical half RGB before/after q33. q33 has its own 3D lookup, probe buffers and atlas/shadow; not the q8 clustered mask. Prior arithmetic/atlas sample findings on other frames do not certify these witnesses. Active contract: the q33 lookup/load/sample/accumulator chain, from exact same pixels, actual descriptors and versions, not new assumptions about direct light masks.
- #762 acquires 23 original guest PC sites including 3D lookup 0x5a4/0x600, records 0x618/0x708/0xb44, both dynamic 8-corner loops and actual image_store 0x1174. Original q8 pair is transferred to actual q33 queue tile by unique membership, not picked independently. Six pre-dispatch buffer inputs (18MiB bounded), scalar operands, exact per-site coordinates/descriptors/samplers, wave masks and native HDR two-pixel before/after. Post-dispatch footprint readback restricted to same read-only native image identity/version and non-aliasing outputs; records 3D texel bytes, interior screen/atlas/shadow 2x2, actual sampler metadata and atlas writer candidates. Unsupported cases explicitly INCOMPLETE. Original reference is conditional on actual sampled bytes; independent footprint/record comparison and arithmetic replay are separate. No visual fix claimed. Global P8 remains historical BLOCKED [0,16]; two newly closed local producer edges, no global cut.


[Detailed #762 contract](DEMONS_APK_762_V4_INDIRECT.md).


### Additional offline verification on the real #760 consumer

- Original guest ISA with the corrected SDWA sign semantics was stopped before actual buffer load PC 0xcf8; both captured lanes independently compute v47=39296 and the exact four consumed descriptor dwords. No observed index injected. The host replay now reports this checkpoint and rejects a mismatch before certifying the conditional shader. This host-only improvement does not require another native APK.
- Actual wave's 4 unique consumed indices: 39296=1, 39297=0xf, 54800=0, 54801=0. Independent native comparisons matched all 64 prefix loads/tile indices/coordinates, 64 stencil, 64 depth, 192 material loads, and 128 outer/inner mask loads. Slot2 authoritative backing remains byte-identical.
- End-to-end local comparator controls reused the real #760 consumer and injected clearly synthetic producer records only: MATCH_CONSUMED for equal output, CHANGED_CONSUMED for a mutation of dword 39297 (31 vs actual 15), reject missing/stale writer serial, wrong native binding and out-of-order ticks. These controls exercise the linkage and do NOT substitute for the actual #761 writer capture.
- Whole material population in this frame: 1805 admitted pixels; mean HDR luminance after q8 2.6575293e-5, after q33 2.6579838e-5, zero pixels >=0.006. q8 covers 464 of those, so neither witness nor queue is a complete lighting oracle. Cluster masks are shared by the lighting variants. P8 global remains open.


## #760 real / #761 — last clustered-light mask producer -> q8 (27/11/2026)

- ZIP 20261127-195446 SHA256 473cd86ccb0fdd085f464bb8d4efb551e39e5f777b75b592d4f61def4f237263; core b126d63; frame359 submit10707 P8_USE_86, tile13498 WG0, lanes44 (788,541) and52 (788,542), stencil86. All 66 manifest entries length and SHA256 verified. 16 q8 tiles, 464 admitted nonzero-material pixels, 0 incoming luminance, 395 nonzero afterward, zero >=0.006, max 0.00390217781. Relative control is not a healthy reference.
- Independent actual guest ISA replay, corrected SDWA sign semantics (16/16 lane/condition controls): 20/20 dynamic sample sites exact coordinates/descriptors/samplers; final RGB/accumulation <=6 ULP dark and <=2 control; all eight light terms within 6 ULP. All 6 entered BRDF slices (474 instructions) same-input <=1 ULP, all 222 live-in comparisons <=8 ULP. No arithmetic divergence, no q8 shader fix warranted. Light0 alone contributes, light1 and3 have zero attenuation, light2 category rejected. Native sample results remain conditional inputs. Local closure, no global P8 cut.
- Mask slot3 guest 0x20ae6d070, 351424 bytes / 87856 words, native 17297821438182927471 offset168048. Both witness cluster bases 39296, outer=1 inner=0xf, waves also read cluster54800 with inner=0. All observed mask words match retained native bytes. 34 bindings in full-frame P8 journal are read-only. Legacy DS_BUFFER_PRODUCER logs repeatedly identify direct shader 0x1ce1a858f62b5a11 writing this exact guest range (slot2), including preceding frames. Its slot3 summary 43928 bytes, 8:1 ratio. Legacy f481 producer/consumer payloads differ in 54/87856 words but lack a shared writer serial and are recorded at different points of the cycle; not a corruption proof. #760 full journal lacks writer because it is normally in preceding frame; producer guest binary not retained.
- #761 active bounded contract: acquire exact direct producer in both EvidenceFrame frames, all 4 pre-dispatch buffers and 2 post-dispatch written ranges, actual launch, scalar operands, guest binary. MarkUse makes guest code mandatory in exporter closure. Link last overlapping bound writer by serial/frame/submit/range to actual q8 slot3, compare exact consumed dwords independently of full-buffer changes. Capture state/ticks/native identity and transfer candidates; MATCH_CONSUMED closes only publication of observed words and leads to original producer ISA/input analysis; CHANGED_CONSUMED identifies exact boundary to investigate, not necessarily illegal write. INCOMPLETE rejects missing serial, mismatched/truncated payload, out-of-order tick/dispatch, missing code and unsupported layout. Never assume native producer output is correct guest assignment.
- Producer-specific cap 12MiB copies / 8 dispatches, individual 2MiB; existing raw critical caps still apply. No new GPU arithmetic/brightness changes. Retain schema4 q8 witness for precise indices; do not repeat BRDF investigation unless new evidence. Actual #761 device capture pending. P8 historically BLOCKED [0,16], no global cut.


[Detailed #761 contract](DEMONS_APK_761_V4_CLUSTER.md).


## 759 oracle correction — supersedes the provisional BRDF divergence (27/11/2026)

- Same ZIP 20261127-183012, SHA256 a32cfbdc07d0579642bc6894caa12f87f9b7c3bac97f5c4c620b1b300f4263c2; frame475 submit14104, tile16287 WG0, lanes31/39. Corrected offline reference: **MATCH_CONDITIONAL for both**, final RGB ULP [3,0,2] and [0,1,2], all 8 individual light terms <=3 ULP, ten sample addresses/descriptors/samplers per lane exact.
- The previous zero prediction for light1 was an oracle defect: original SDWA V_CNDMASK_B32 at PC 0x2e8 ignores selected source NEG in the independent executor. Actual encoded comparison (s69=2047, v2=2812) and negative v11 produced wrong negative sign rather than positive. Verified exact sign-bit XOR followed by ordinary CNDMASK on lanes 0/31/39/63, both condition outcomes. Guarded exact two sign-select encodings at PCs 0x2c0 and 0x2e8, using verified-unused scratch v100. No Android shader edit. The host comparator now runs semantic controls before replay.
- #760 native BRDF schema4 was built from b126d63 before this late counterproof. Its instrumentation is not a demonstrated rendering fix and is not necessary to retest #759's already matched arithmetic. The host-only correction applies to both schemas 3/4; no extra APK compilation for the host fix. The provisional BRDF claim below is historical, not current.
- Next active contract: provenance of light assignment / sample input (not the already matched accumulator). Actual q8 mask slot3 guest 8772865392, native 8932172584204275344, offset 167280, bytes 351424; all captured bindings to the exact range in this frame are read-only. The writer is not yet located; do not assume absence of light4 is a defect. The q8 witnesses visit only indices 0..3, while light4 is present in other cluster words and absent in these witnesses. q0 excludes their tile and q33 does not change their HDR. This is an upstream lead, not proof of missing intended light.
- Global P8 remains BLOCKED [0,16]; exact local match only, no inferred half cut or healthy reference. Prior findings retained in scope.


## #759 real / #760 — BRDF instruction-local contract (27/11/2026)

- Source capture SHA256 a32cfbdc07d0579642bc6894caa12f87f9b7c3bac97f5c4c620b1b300f4263c2, revision 53860bf, frame475 submit14104 tile16287 WG0, lanes31 (703,651) and39 (703,652), stencil86. 32/32 manifest payloads verified. q8 174 unique tiles and 8398 admitted material pixels, all HDR zero on entry; only 1 pixel reaches descriptive L>=0.006 after. Relative control is NOT a healthy control. L after: 6.2463879585e-6 and 8.7742447853e-6.
- Local matches: 64/64 lanes for tile coordinates/stencil/depth/material and masks, native HDR read/add/store, four category decisions per witness and 24 dynamic scalar fields each. Both original ISA replays visited ten identical sample coordinates, descriptors and samplers (including previously missing dark 0x1e8). Observed image results are conditional reference inputs; no independent texture sampler or global prefix claim.
- Original conditional replay yields zero light1 RGB where Android yields [9.1452366e-6,5.7600209e-6,3.0205415e-6] and [1.18807575e-5,8.30620866e-6,4.54074916e-6]. Lights0/2/3 match zero. Reference checkpoints for lights0/1/2, both witnesses, at 0x194c,0x1978,0x19b4,0x1b0c,0x1b18,0x1bb4: all 36 bitwise equal. Difference is downstream BRDF or an unmeasured live-in; original light1 N.L before clamp 0x1c94 is negative in both, not yet known on Android. The sign of the contribution mismatch does NOT explain the overall darkness. Do not equate an unexpected tiny positive contribution with cause of missing radiance.
- #760 schema4 appends bounded 37 consumed live-ins, scalar VCC literal and 79 per-instruction results in [0x1bb4,0x1d50] for each entered light and witness. Total 3,986,112 bytes, preserves schema3 offsets and older readers. Distinguish same-input first divergence from entry-register producer discrepancy. Original ISA replay exports the entry and independently executes each original op with its observed input state; only offline reinjection, no changes to production lights. Final three results must bind exactly to the existing accumulation term. Missing markers, misbound RGB/term and missing reference exports fail incomplete. Local original-ISA roundtrip on fingerprinted guest slice: 79/79 exact; mutated 0x1c94 first mismatch, missing marker rejected. CI tests layout, equality/divergence and failure controls. Android execution pending.
- Active V4 contract only: BRDF entry/operation -> term. First same-input mismatch: reduce its instruction, validate effect, then correct. If operations match but entry differs: follow the actual producer of the differing live-in, not accumulation or mask. If original and native path differ, prove the first branch with the exact operands. If no valid witness, repair acquisition. No synthetic brightness. P8 global remains historically BLOCKED [0,16]; no new global cut.

[Detailed #760 contract](DEMONS_APK_760_V4_BRDF.md).


## #758 real / #759 paired V4 contract (27/11/2026)

- ZIP 20261127-163934, SHA256 8edd69c67c099d3e3c75189ba270c893b7ffc34b828178a86705360db56a2fec,
  revision 3a2da6e, frame451, submit13436, P8_USE_93, tile16694, WG17.
  Slot2 publication 86016 bytes identical to stable guest backing;
  24/24 admitted dynamic fields and four category decisions match in lane2.
  Prior mask, load and accumulation checks remain scoped to the captured wave.
- Real defect in witness selection: the largest packed material RGB is a lit
  control, not the dark target. Material population 4408; 8/4408 have HDR
  luminance >=0.006 before q8; 12/4408 after q8. Contrast is already present:
  lane0 (752,664) L before=0, after=0.00000402110815;
  lane2 (754,664) L before=0.0746182434, after=0.0749275818.
  Same stencil 86; 99.58715% of final control luminance is incoming.
  The q0-after snapshot and q8-before payload are byte-identical; tile16694
  is not in q0, only q8 and q33. The earlier HDR writer is not proved.
- Offline original ISA conditional replay of #758 lane2 checks ten dynamic
  image reads (all coords/descriptors/samplers match exactly), final RGB
  bitwise, accumulated RGB <=2 ULP, and four per-light RGB terms <=4 ULP.
  #757 selected pixel (882,669) also replays with <=2 ULP
  (use actual launch lane in the archive). Reference inputs are observed
  samples and verified wave reductions, not independently sampled textures.
  Dark lane0 replay checks the first five loads and reaches missing sample
  PC0x1e8. It cannot complete from #758. Mutation of 0x1e8 coordinate,
  missing sample, and altered light2 term distinguish candidate divergence
  from incomplete, without using observed output as reference.
- #759: same q8 dispatch/workgroup; choose two admitted material lanes of
  same stencil with dark incoming HDR and strongest contrasting control.
  Tested selector on real #758 yields exactly (752,664)/(754,664).
  No new dispatch, synthetic light, or global P8 certificate. Schema3 shares
  64-lane fixed/mask fields and stores separate term, sample and light-record
  tails (3,631,296 bytes). Both paths use the same original guest binary
  and native operand versions. Correct the subnormal half comparator to
  accept adjacent rounding while treating non-adjacent zero flush as
  ambiguous. Actual Android result pending.
- Next: paired local divergence -> isolate and correct that operation;
  conditional arithmetic MATCH -> trace exact differing sample source or
  upstream incoming HDR writer, not repeat accumulator/publication tests.
  The contrast thresholds are descriptive, not expected guest brightness.
  If no absolute lit control exists, the relative-control flag explicitly
  says so; do not call it a healthy reference. P8 global still historically
  BLOCKED [0,16], no fictitious global cut.

[Detailed contract](DEMONS_APK_759_V4_PAIR.md).

## #757 — resultado real / contrato ativo #758

- ZIP 20261127-161050, SHA256 c6cde2375d5082e261711c022880d328dac0f499c73b6ae955151f60232a5e0f. Frame 383, submit 11412, P8_USE_84, q8 WG287 lane42, pixel (882,669), stencil 0x56, tile 16710.
- MATCH local: 64/64 coordenadas, stencil, depth; 192/192 leituras de material; 320/320 operações de máscara; 9/9 acumulações; 576/576 operações HDR RGB compatíveis. Não elimina outras waves/filas ou amostragem independente. P8 global permanece historicamente BLOCKED [0,16], zero metades globais eliminadas.
- Replay condicional da ISA original, com valores de amostras observadas e reduções já comparadas: RGB a até 2 ULP float32, mesmo FP16. A luz 0 possui RGB 0.00828048; luz 1, raio ~1.35 contra distância ~1.467; luz 2, flag 0x21 contra bit de categoria 0x20. Essas explicações intermediárias são derivadas da referência, não captura nativa dos loads escalares. A soma baixa não demonstra defeito de iluminação intencional.
- **Único contrato ativo #758:** backing guest estável e autorizado -> slot 2 (224 registros de 384 bytes) -> loads escalares dinâmicos da luz e decisão AND/EXEC. Capturar a mesma fonte antes/depois da cópia e após dispatch, propriedade/geração/offset, e registros realmente consumidos (inclusive rejeitados). MATCH encaminha ao produtor dos valores; DIVERGENCE no campo consumido autoriza localizar/corrigir a publicação ou load; INCOMPLETE não certifica autoridade. Sem alteração artificial de luz.
- Ver [DEMONS_APK_758_V4_LIGHT_RECORDS.md](DEMONS_APK_758_V4_LIGHT_RECORDS.md). #758 preparada, resultado de aparelho ainda não existe.

## #757 — V4 q8, aquisição diagnóstica

Implementado o contrato local de admissão, máscaras e acumulação/HDR da fila 8.
Sem corte global e sem correção visual alegada antes da captura real. Ver
[DEMONS_APK_757_V4_Q8.md](DEMONS_APK_757_V4_Q8.md).


## #756 — revisão V4, 27/11/2026 (não é outra build)

Zero corte global; P8 não é mais pré-condição operacional. A prova real
reproduz 41/41 VS (83.190 invocações, 332.760 componentes numericamente
iguais), 1437175/1440000 pixels da máscara do céu e seu complemento
material de 2825, sem divergência de máscara. A versão 48781 é a mesma
nas filas 0/8/33; q8 tem 2726 pixels de material nos seus tiles, q0 99.
O bit64 de stencil está presente nos 2825. 2822/2825 têm radiância abaixo
do limiar descritivo 0.006 antes do pós. Próximo contrato ativo: q8
workgroup0 tile13899, local7,1, shader 3dfe402653925fbf. Ver
[DEMONS_V4_LOCAL_CONTRACT.md](DEMONS_V4_LOCAL_CONTRACT.md). Correção do
oráculo no host não é fix de produção. Sem APK #757 nesta revisão.


## Preparação #757 — executor ISA e suporte causal, 27/11/2026

**P8 BLOCKED [0,16]; nenhuma metade global eliminada. #757 não disparada.**
A referência offline agora executa o shader guest 3265b01ed00a5c01 com
trampolins de diagnóstico para MIMG e amostragem lógica independente do
tradutor SPIR-V do Android. 64/64 pontos distribuídos e 4/4 pixels do quad
central coincidem bit a bit com o flip real de #756. O pixel (960,540)
depende de 16 texels da cena e um do overlay. Substituição sintética fora
desse suporte não o altera; dentro altera. O motor bilateral executa ambos
os resultados de teste através do compositor real: FAIL [0,8] e PASS
[8,16]. Os produtores anteriores nesses testes são sintéticos e não
certificam o jogo. Rejeita censo ausente, fronteira errada, observação ausente
e diferença fora do suporte. O executor também passa teste de leituras
dinâmicas repetidas no mesmo PC, preservação de SCC e rejeição de dados
incompletos. Sem alteração do pipeline de jogo.

Lacuna concreta ainda aberta: P8_USE_242/243 não têm operandos escalares,
imagem por versão e cobertura de primitiva completos. O segundo usa
arrays/3D/LOD ainda não implementados na referência de amostragem.
O prefixo real C1–C8 permanece sem autoridade independente; portanto nem
PASS nem FAIL reais podem ser inferidos dos testes. Não gerar APK que
apenas colete mais logs sem completar os dois caminhos. A instrumentação
ISA é host-only, não altera o shader do jogo. Ver
[DEMONS_P8_757_PREPARATION.md](DEMONS_P8_757_PREPARATION.md).

## Atualização prioritária — captura #756, 27/11/2026

**P8 BLOCKED [0,16]; zero metades globais eliminadas. Não disparar #757.**
A captura real frame427 já foi analisada. O replay analítico do compositor
reproduziu 2066048/2073600 pixels exatamente, restantes com diferença máxima
1 LSB RGB. A testemunha final (960,540) tem baseline e observação iguais;
a sensibilidade à cena foi exercitada. O próximo passo é fechar o suporte
causal dos escritores da cena P8_USE_242/243, não repetir certificados locais.
O método inverso, resultados, limitações e condições bilaterais estão em
[DEMONS_APK_756_RESULTS.md](DEMONS_APK_756_RESULTS.md), que prevalece sobre
os estados históricos abaixo. A referência analítica não certifica DCC ou
P8; nenhum APK foi gerado.


Atualizado em **27/11/2026**. Este registro responde, para cada build: **o que
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


### #750 real capture 20261127-012846 / next build
- Selected producer frame 383 (51 draws, 41 native position records), first consumer frame 384 submit 11608, owner 0x287ae0000 version 43990 write_order 182409. 624/624 required files, continuous 1024-event journal. Previous frame-boundary retention defect closed.
- Independent POS0 reference: 41/41 observed draws MATCH, 60,076 vertices / 240,304 components, 0 divergent. Material VS13f07 conditional 0x74..0xc4 tainted; EXEC restored 0xcc; duplicate descriptors accepted only when both complete native observations have identical ranges and bytes. Two unobserved Z=0 occluders have reference positions but no host export comparison; six indirect draws have zero instances; two auxiliary draws unsupported.
- Draw 599 reduces colored layer0 2276->441; independent background prepass raster reproduces exactly 1,439,559 changed D32 pixels and preserves 441, max abs D32 error 3.875e-10. Guest PS552d exports ff000000 for captured zero RGB operands. No sky/material forced color. Deferred q0/q8/q33 HDR 3/406/433 nonzero RGB, all within the 441-pixel mask.
- Critical missing part of final-symptom witness: a direct HDR graphics pass VS00d5229e9ce98fd9 PSab81bfa8ee4e0fee, frame383 draw52 passed 1,440,000 samples (before retained depth clear119). Corresponding frame384 P8_USE_93 writes HDR after q33; input version8620, next HDR target version8622, later post readers version8628. Raw capture had no full HDR output for this pass or last HDR consumers, so prior correlation cannot certify P8. The observed pass is part of the same final-output chain, not an unrelated case.
- Next bounded acquisition: exact before/after direct HDR pass, pair linked to cut_use, actual blend/clear/write/depth state, small dynamic forward image operands, final HDR readers and actual compositor scene input. Raw caps remain 192MiB stored / 256MiB transferred. No GFXReconstruct, no blanket shader override.
- Global cut: none. Local POS0/raster matches narrow candidate operations, not entire C1..C8. Independent complete P8 expected-image backend remains absent; new acquisition addresses an actual missing final-contribution edge rather than treating a native snapshot as a guest oracle. Verify same frame, versions, and actual output; distinguish missing prepass input, dark forward output, and subsequent loss.

### #751 delivery verification — 2026-11-27
- Build 751 / versionCode 851 / commit cf4dfb59f5f7e8bdaf3bb971b45bd4ed1895a524; run 45941006468 completed success; artifact 15172422561.
- APK 18,496,294 bytes; SHA256 03537bf86f109c35b7a1ea49465c9809474cc432b1e7b5f596b32f8d3a3756fd; original signing certificate SHA256 8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0. Verified APK v2 RSA PKCS1 SHA256 and signed-content digest, ZIP integrity, ARM64 and embedded source revision. Focused DS/UFC checks and Android build passed; optional full native regressions skipped.
- #750 numeric comparison clarification: 240,304 components matched under the oracle's numeric equivalence, 226,584 bit-identical; 41,120 matching NaN components, of which 13,720 had differing payloads. Four additional draws are POSITION_REFERENCE_ONLY (129,138,362,610), not native matches; six are ZERO_EXECUTED_INVOCATIONS. Nine indirect-fixup observations independently reproduce 42 DWORDs; no journal sequence gaps. CPU fallback stream records remain incomplete, not a proven stale-argument fault.
- Host tool docs/android/tools/p8_hdr_lineage.py (e7962019) checks actual radiance payload hashes, command association, image identity, subresource and observed versions; synthetic positive and version-mismatch negative cases exercised. #750 lacks the new boundaries and reports OBSERVED_CHAIN_INCOMPLETE as expected. Estimated transfers using #750 exact observed views and sizes: 166,304,640 bytes including new snapshots, below existing 256MiB copy cap; actual #751 run may differ and storage/copy limits remain enforced.
- P8 interval [0,16] unchanged; no global cut. #751 must be tested on device, same scene and diagnostic mode F. New full HDR snapshots and selected dynamic images are not an independent full guest render or a lighting fix.


### #751 actual capture / selected-cycle CP reference (2026-11-27)
- Capture 074347: target frame 408, q0 submit 12254 consumes G-buffer v42254; producer frame 407, clear draw112, last draw622. 53 retained cycle draws, 43 position artifacts, 238 manifest-required paths. At threshold luminance >=0.006, full HDR q0/q8/q33 have 0/4/4 lit pixels, final HDR input to post has 4, compositor input 86. Scene is already dark before Android presentation. Albedo nonzero RGB 1498; nonzero RGB and lit count are different (tiny off-mask HDR values exist). No global P8 cut: darkness of observed input does not certify all guest semantics.
- Critical cross-run observation: #750 command49 frame383 submit11578 args 0x21de00078, same index base 0x2e8100000, limit 22020096, stride20, target/shader masks and targets as #751 command49 frame407 submit12225 args 0x21de07c38. #750 child51 emits 92160 indices, draw52 PS ab81bfa8ee4e0fee executes; #751 children51/53/55 each emit zero indices, no draw_prepared, and PS is absent from selected HDR frame. This explains the observed missing pass, not whether the guest legitimately emitted zero; the two runs are not a same-input oracle. The batch precedes the retained deferred clear, so do not misattribute it to the later G-buffer producer.
- CP witness #751 retained 4 old addresses, last observations frames283-303, 66593608/67108864 blob bytes and 79 blob drops. All 5 retained batches had pipeline_on=true, fan_on=true, native_state_supported=false. The actual relocated batch is absent. Earlier pinned producer outputs include valid zeros and nonzeros; not proof of stale read in this episode. NaN input positions of draws230/266 revalidate prior W0 findings (#730-737); no new producer divergence demonstrated and no NaN patch.
- Change: selected-window CP batches capture at actual address independently of historical pins, indexed fixed-count 1-3 children (observed 3 and 1), 64 batches, separate SHA256-exported JSONL. Front ownership reacquired after queued child before cache access; copies frozen after consumed fields and before next child; final GPU wait only after all original children. The F path never substitutes counts or changes render state. Exact authority/identity/operands and missing coverage reported; historical slot epochs are not asserted for new addresses. C1 explicit coherence experiment now drains prior queued child before touching renderer cache; this corrects a source-ordering gap in the experiment, not a confirmed lighting fix. No global P8 decision without independent comparison. Next observation: distinguish actual GPU zero from CPU consumption mismatch on the batch which removes the direct HDR pass, then investigate the exact producer only if the reference agrees. No whole-state capture.

### #752 delivery verification — 2026-11-27
- APK build 752 / versionCode 852 / package com.kytyps5.plus / source revision b167ce99f4eb5b126df066b7ea0224c5e98b2ced. Workflow [45986195121](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45986195121), artifact 15192071975.
- APK 18,496,866 bytes, SHA256 76f2a2bffeb0bac54bd2aa3c329d4c7ac1efa6db8a1b6e6e370834289218dcfd. Artifact ZIP SHA256 c18c8fd40324f200b862f2bf728fe6e732d4865a6c85e6b43de4206089aa8145 matches GitHub. APK v2 RSA PKCS1 SHA256 signature, signed-content digest, ZIP CRC, certificate SHA256 8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0, ARM64 ELF headers, embedded revision and Android manifest verified. Focused CI checks passed; optional full native regressions skipped. Artifact generation completed without reported failure.
- Offline selected-cycle comparator in 82458dbf, archive transport regression in 87ae1553 (documentation/tools only; APK revision unchanged). Seven local comparison cases passed. Full synthetic three-child ZIP with manifest/digest and original journal binding independently reports first child DIVERGENCE, other two MATCH. This is comparator validation, not a gameplay result. #751 ZIP does not include new ordered JSONL.
- Global elimination: none, interval [0,16]. Local native position reference 43/43 MATCH, 75,064 vertices, 300,256 numeric components, 283,016 bitwise identical. The missing forward pass is linked to zero emitted indices in this #751 witness, not yet to proven incorrect values or an identical-input expected final image. #752 fixes capture and C1 ordering only. Await same scene F capture from device; no new claimed visual fix, no forced counts, no full independent P8 PASS/FAIL certificate.

## #752 — captura real 082502 (27/11/2026)

- Revisão b167ce99, modo F, arquivo KytyPS5-Plus-diagnostics-20261127-082502(2).zip. Closure 623/623 presente; manifest P8 39/39 arquivos com tamanho e SHA256 conferidos. Boundary frame384, submit11367, q0, owner 0x287ae0000, version38654, write_order177795. Global P(8) BLOCKED, intervalo [0,16], zero metades eliminadas.
- Replay offline com prelight_vertex_oracle.py (blob bb859f018da8da2827beb44fef39c64659c9fac1): 39/39 draws comparáveis MATCH, 59060 vértices, 236240 componentes numéricos, 222520 bitwise iguais. Revalidação de POS0 nas invocações efetivamente admitidas; não certifica cobertura esperada do cenário nem todos os estágios do VS.
- Mesmo lote 0x21de07c38: frame383 submit11350 emite 1035/0/0 índices; frame384 submit11367 emite 828/0/0. Todos os 10 registros retêm cinco DWORDs, repetição e emissão compatíveis; original comparador retorna 5 MATCH, 5 INCOMPLETE (três lotes frame384 com parent_command=0). A referência de todos é backing posterior. O primeiro filho de cada lote de três muda GPU-dirty exato de 20 bytes para backing limpo durante os loads originais. Não há bytes nativos anteriores à publicação. Corrigido comparador para marcar MATCH de referência posterior sem autoridade prévia como INCOMPLETE, preservando post_read_reference_match e offsets perdidos. Resultado corrigido: 4 MATCH locais / 6 INCOMPLETE / 0 DIVERGENCE. Sete testes originais, transporte três filhos, rejeição ZIP corrompido e quatro regressões novas passaram localmente.
- Mesmo frame384 submit11367 P8_USE_93 VS00d5229e9ce98fd9 PSab81bfa8ee4e0fee, 828 índices. P8_FULL_9.bin -> P8_FULL_29.bin, versões 7717->7719, ticks 38127->38128: 1.440.000 pixels HDR; RGB bitwise idêntico em todos, alpha muda em 1.439.563. Layers 2..5 do G-buffer têm, cada um, 1.439.560 pixels no valor dominante (0xff000000/0xccff7804/0x00f00001/0x0000fe80), 440 fora. Os 437 pixels cujo alpha permanece inalterado pertencem à região não dominante de cada layer. HDR q0/q8/q33: 0/1/1 pixels acima de 0,006; entrada pós-processamento 1, compositor 7. A ausência TOTAL do passe direto não é condição necessária do sintoma: ele executa nesta testemunha escura. Não prova contagem correta, shader correto nem primeira divergência de material.
- Bloqueios concretos: DsGuestCommand usa Phase(frame), que retorna -1 após boundary_reached; CP selecionado continua até fim do frame. Corrigir política para reter o diário compacto de parent/child após a fronteira (sem expandir a fase pesada), incluindo relevância HDR. A referência CP precisa preservar a autoridade nativa antes dos loads originais e comparar publicação/consumo com bytes de mesma versão, sem substituir contagens; a identidade do buffer e a ordenação precisam ser verificadas. Pins históricos em 0x21de00078/138/d8/198 não incluem endereço corrente 0x21de07c38; blob 66742440/67108864, 66 drops, 96 drops de eventos. Os produtores selecionados aparecem em metadata (71e0e2994d683e35, 7d5ad40fca3137e2, ea2e66ea6ad76baf), sem operandos retidos do caso corrente. Não usar snapshots de frames277-280 como referência do ciclo383-384. Reter cone do produtor real no ciclo anterior, com orçamento reservado e falhas explícitas.
- Próxima decisão: demonstrar contraexemplo independente C1..C8 com efeito causal no G-buffer/HDR da mesma testemunha (FAIL) ou certificar prefixo e divergência posterior (PASS). O comparador local corrigido impede falso certificado, não é mecanismo bilateral global. O replay nativo do passe não pode usar só as imagens observadas: seis buffers e parte das imagens lidas não têm payload, e falta a referência independente completa. Não abrir outra classe por correlação. Não iniciar novo APK apenas por esta alteração offline/documental. Sem correção visual confirmada.


## Aquisição seguinte — reparo da autoridade pré-leitura e dos pins
A #752 demonstrou perda de autoridade GPU durante o primeiro load e
perda do vínculo parent/child depois do boundary. O núcleo agora separa
CommandPhase (diário compacto de ambos os frames, incluindo HDR) de Phase
(produtores pesados até o boundary). Vincula P8_USE e radiance_draw_pair
ao comando. Preserva dois readbacks de 20 bytes por filho: autoridade
nativa pré-leitura e referência pós-leitura; nenhuma espera diagnóstica
entre filhos, coleta depois da barreira dos loads reais. Comparador
verifica identidade, GPU generation, bytes efetivamente GPU-owned,
ordem e publicação, rejeita referências ausentes/instáveis e distingue
MATCH e DIVERGENCE local sem promover a decisão a P8.

A aquisição de produtores retira pins históricos no início da janela e
arma os dois endereços recentes do stream de três registros, com orçamento
de 64 MiB para dois conjuntos atuais de até 16 MiB e suas cópias pinadas.
Preserva a cadeia de candidatos até o consumo real, não reinicia a cada
alteração. O endereço realmente selecionado é comparado de modo independente,
mesmo se não estiver nos pins; sua lacuna de produtor permanece explícita.
Sem força de contagem, sem troca de luz/depth, sem camada whole-state.
Os testes exercitam transição de autoridade, diferença de bytes, falta de
referência, versão/identidade/ordem, child de outro submit, MRU e orçamento.
**Corte global nenhum; P8 [0,16].** O mecanismo local de publicação pode
decidir uma primeira divergência operacional; a contribuição no mesmo
G-buffer/HDR e o prefixo completo ainda exigem prova. Não confundir
esta aquisição reparada com solução visual ou certificado global bilateral.


## #753 — APK compilado, assinado e entregue; aguarda captura
- Revisão nativa: `96ce5f8b145a3e01c8e1cc47e5e307ffc6f97fac`; versionCode 853, ARM64.
- CI [45995977982](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45995977982): testes focados e build concluídos com sucesso. Regressões completas opcionais não executadas. Oito novos testes de referência pré-leitura, sete testes CP anteriores, quatro casos de transição de autoridade e transporte de três registros aprovados.
- [Artefato 15196121853](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45995977982/artifacts/15196121853), APK 18.500.418 bytes, SHA-256 `7cc79207fd209140f1ee8de00a7894d78103aab42046adb421c2459af1b36c32`; ZIP SHA-256 `9646a894f1fb782b2494b4e9129ea7cf259866883356078dd6dfbb18842a5c70`. Ambos conferidos após download. Assinatura V2 e digest assinado verificados independentemente, certificado estável `8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0`. Revisão completa encontrada em libmain.so e native-core.json. CRC do APK íntegro. Os hashes do staging não são hashes das bibliotecas após stripReleaseDebugSymbols; o APK final confere com o SHA-256 auditado pelo CI.
- **Cortes globais novos: 0. P8 [0,16].** Certificados locais #752 preservados; nenhum resultado de gameplay #753 foi presumido. A captura anterior não tinha os bytes GPU pré-publicação, perdeu identidade de comandos após boundary e reteve pins históricos em lugar dos produtores atuais. A nova build corrige essas deficiências de aquisição. A próxima captura em modo F, no mesmo save/câmera/configuração, deve verificar identidade, autoridade, cadeia de produtores e o vínculo ao consumidor do G-buffer/HDR. Uma divergência de publicação comprovada deve ser corrigida, com efeito causal demonstrado; MATCH local não constitui PASS do prefixo C1..C8. O instrumento bilateral global P8 permanece incompleto. Nenhuma mudança artificial de iluminação, profundidade ou contagem é considerada correção.


## #753 — análise do diagnóstico 092211 (27/11/2026)
- Revisão confirmada 96ce5f8; 39/39 arquivos P8 íntegros. Fronteira frame359/q0/submit10692, G-buffer v38650, ordem169389.
- Todos os seis batches têm vínculo pai/filho; dez registros comparáveis, 10 MATCH de consumo/emissão; dois primeiros registros GPU-authoritative verificados antes/depois da publicação. Frame358 batch 153600/0/0; frame359 batch 86016/0/0. Não confundir diferenças entre frames com erros para a mesma entrada.
- 41/41 draws com POS0 comparável: 60.076 vértices, 240.304 componentes numericamente coincidentes (226.584 bitwise). Dez draws sem comparação POS0 nativa completa continuam explícitos.
- Produtor seq7069, frame358 submit10675, shader 7d5ad40fca3137e2, entrada de 44980 primitivas e 703 grupos. Replay matemático do slice real de visibilidade/tesselação, com os nove mips HZB zero e constantes/descritores do mesmo dispatch: 28 primitivas passam, todas nível5. Deltas 28/7840/15708/86016 reproduzem contador global e contadores do registro334 observados na entrada da compactação seguinte. 86016 coincide com a saída nativa e a leitura CP da mesma cadeia. Isso não certifica os 56 bytes de cada registro de trabalho, os índices/vertices gerados, os produtores da HZB, reset completo ou toda a compactação. Sem erro comprovado no contador/publicação dessa testemunha.
- Mesmo passe HDR frame359 submit10692 command1035, VS00d5229e9ce98fd9/PSab81bfa8ee4e0fee: 86016 índices, RGB muda em 429 de 1440000 pixels; alpha muda em 1440000. Não culpar ausência total do draw ou forçar contagens.
- **Corte global: 0; intervalo [0,16], P8 BLOCKED.** Novo certificado local: CP completo e acumuladores do produtor 7d5a no escopo comparado. O passe HDR não fazia parte da aquisição POS0 anterior: índices acima de 65536, bit31 seleciona registros de tesselação, janela única impossível para IDs mistos, e diário de produtores pesado sela antes do consumo.
- Próxima aquisição limitada ao draw HDR real: preservar índices nativos completos, dois intervalos disjuntos de VertexIndex, campos POS0 do próprio draw, escalares consumidos e fatia de 12 bytes por índice gerado (faixa mínima incluindo eventuais lacunas), demais descritores VS/PS dentro do orçamento raw. Registro separado no radiance_draw_pair; hashes e tamanhos no manifesto P8. O recorder continua limitando 65536 vértices armazenados no total e não altera argumentos/saídas da renderização. Testes de mapeamento para IDs mistos, limites de sinal, offset negativo, rejeição de janela excessiva e validação SPIR-V obrigatórios.
- Limitação: não existe ainda prova bilateral do P8 integral. Esta captura visa a próxima aresta concreta de geometria/posição; não é por si uma decisão global nem correção visual. Se a referência de POS0 divergir, demonstrar seu efeito; se coincidir, seguir os produtores reais dos registros gerados ou o consumidor, sem repetir CP/contadores.

### Replays locais reproduzíveis da #753 e comparador da #754
- `python3 docs/android/tools/p8_tessellation_count.py <zip> --seq 7069`: executado sobre 092211; [28,7840,15708,86016] esperado e observado, 28 níveis5. Exige fingerprint da ISA, blob FNV, identidade do consumidor, descritores e HZB uniformemente zero; não usa contagens observadas para gerar a previsão. Dois modelos de MAD concordaram nas decisões desta entrada. Não certifica todas as operações ou outras configurações.
- `python3 docs/android/tools/p8_forward_position.py --selftest`: passou fixture com IDs originais e gerados, operandos de posição e deslocamento, igualdade e mudança do recorte homogêneo. `python3 docs/android/tools/p8_forward_position.py <zip>` compara o slice POS0 da ISA exata 00d5229e9ce98fd9 aos valores nativos por invocação, valida manifesto, descritores, janelas e associa índices a triângulos. Duas semânticas de MAD são explicitamente consideradas; divergência candidata não equivale automaticamente a erro de ISA nem a diferença de cobertura. ZIP 092211 retorna INCOMPLETE por ausência de geometry. A nova aquisição resolve essa ausência, sem garantir que todo dado exigido será observável no aparelho. Nenhuma decisão global P8 é atribuída ao selftest.

### Entrega #754 — verificada
- APK nativo 3371565ebbffda6556aa77024dbe5079d5cdaded, versionCode 854, 18.510.062 bytes.
- CI [45999776729](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45999776729) sucesso: regressões focadas, testes de posição/SPIR-V, compilação, testes Android, auditoria de dependências e assinatura. Suite nativa completa opcional não executada.
- [Artefato 15196969721](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45999776729/artifacts/15196969721). SHA256 APK c301b3d0164d752c0254c2ead332a841d44562e037f1627e1da257fffb325a88, ZIP ee50e845b6e4b0201c5f110ced8022e8ecd1d858d8dca867e8684f88fe5ef01b. Download, CRC, assinatura V2 e digest criptográfico, certificado estável 8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0, arquitetura AArch64, revisão embutida em libmain.so e manifesto nativo verificados localmente. Hashes do staging diferem após strip Gradle, APK final idêntico ao hash CI.
- Replays independentes versionados em d4696fb. Próxima execução no aparelho modo F mesmo save/câmera. P8 ainda BLOCKED [0,16]; nenhuma correção visual alegada.


### Revisão metodológica após #754 (27/11/2026)

O usuário exigiu corrigir a investigação que só produzia certificados locais.
[Motor bilateral e contrato de integração](DEMONS_P8_BILATERAL_METHOD.md):
testados PASS e FAIL em cone finito com execução independente RDNA1 e 18
controles. Inventário real #752/#753: 284/284 leituras, 12 versões,
288 operações candidatas e 540 ocorrências de predecessores não fechados.
O adaptador do cone real, a cobertura dos contratos guest necessários e a
propagação real ao flip ainda não estão implementados. P8 continua [0,16],
zero corte global. Não usar os fixtures como certificação do jogo; não
disparar outra build exclusivamente por mudanças metodológicas ou localizadas
sem mecanismo bilateral integrado. Certificados locais anteriores mantidos.


### Execução gráfica e vínculo real #753 — 27/11/2026

- Integrados `c32a00885ba60f7b258ec4161e7af24d71517b59` e
  `c60b716b9565be1d17db4fbca35a1be67ce12141`: contrato guest_export
  no decisor bilateral; fixture de propagação GFX10 executa PASS e FAIL e
  cinco controles negativos. CI 46020518399 e 46021488616 aprovados; o
  segundo inclui oito rejeições de vínculo incorreto de versão/frame.
- ZIP #753 092211, draw599 (frame358, submit10684): execução RDNA1 do VS
  c96f3fcde8141266 com 19 invocações, 76/76 componentes POS0 bitwise,
  PARAM0 e PARAM1 exportados, nenhum range vinculado ausente.
  Escopo: invocações admitidas; PARAM sem observação nativa correspondente.
- O produtor não está no mesmo frame do consumidor: owner 0x287ae0000,
  versão38650, ordem169389, frame359/submit10692/q0. A identidade completa,
  mip/layer, hash FNV das 1.440.000 palavras e 43 leituras coincidem com
  P8_FULL_2.bin (SHA256 ca8613c494570124631078c23f1bdbe6173c3dc0123bb37f18bd58930bf0c94c).
  É vínculo de versão nativa, não certificado de conteúdo guest esperado.
- PS 552d823dcfcf2dee: a execução integral rejeita acesso de 16 bytes em
  0x207b25ed0; só 12 estão retidos pelo SRT. O quarto DWORD s3 é morto para
  MRT0. O recorte de dependências preserva as instruções originais vivas,
  contrai apenas essa leitura x4 em x2+x1, sem fabricar o DWORD ausente.
  `p8_material_mrt0.py` executado sobre o ZIP real: export esperado
  0xff000000 em três variantes de MODE e dois padrões de registradores
  não observados; 1.439.562/1.440.000 palavras da versão de fronteira
  têm esse valor. 27 fixtures sintéticas com execução do shader integral
  e recorte coincidem; retirada de operando vivo rejeitada. O MODE efetivo
  do PS não está retido. Não é prova de cobertura nem de que as constantes
  RGB produzidas antes desse draw são corretas.
- O compositor real P8_USE_375 lê overlay 0x281a80000 versão57784
  ordem169711 (1920x1080 RGBA16F, sem payload integral), e cena
  0x2b2a40000 versão1114 (P8_FULL_37.bin). A saída flip P8_FULL_38.bin
  versão317 está retida. Falta o overlay completo, os escalares/descritores
  guest do compositor e a reprodução independente dos produtores da
  fronteira e da cadeia até esse flip.
- **Cortes globais: 0. P8 BLOCKED [0,16].** CI host aprovou a implementação,
  não certificou P8 no jogo. Nenhum APK #755 produzido. O contrato do
  cone real e sua propagação não estão fechados; não confundir reparo
  de replay local com correção visual ou autorização de nova build.

Reprodução (ZIP real fornecido pelo usuário, não incluído no repositório):

    python3 docs/android/tools/p8_version_link.py DIAGNOSTICS.zip --draw 599
    python3 docs/android/tools/p8_material_mrt0.py DIAGNOSTICS.zip /path/to/kyty_p8_guest_ref --check-fixtures

### #755 — preparação da aquisição P8 (27/11/2026)

- Captura real de referência: #753, frame 359. Permanecem os certificados
  locais da publicação CP (10/10), contagem 7d5a, POS0 (41/41) e material
  do draw 599. Nenhuma metade global eliminada.
- O APK anterior não retém a origem da HZB (0x2b2360000 -> 0x2b2570000,
  shader dbecbbe2051b41ba) antes da tesselação nem a segunda entrada RGBA16F
  do compositor (overlay 0x281a80000). O novo código captura as duas
  extremidades da redução min2x2, incluindo o frame produtor anterior;
  o compositor retém as duas imagens, operandos escalares e estado de
  raster/export e associa a saída ao flip real. O orçamento crítico separado
  é 80 MiB de armazenamento / 96 MiB de transferências; o orçamento normal
  continua 192/256 MiB. O censo PM4 original registra pacotes, predicação,
  intervalos condicionais ignorados e a identidade transmitida à thread
  de registro, com limite explícito de 16 MiB.
- Host: `p8_paired_replay.py` valida hashes, identidade/ordem, formatos,
  descritores e versões de entrada/saída. `p8_bilateral.py` recebe os contratos
  HZB min2x2 e compositor RDNA1. CI 46025386665 passou; fixtures locais com
  o binário original do compositor executaram 64 pixels, claro 0xe2589625,
  escuro 0xc0000000; falha introduzida no prefixo selecionou [0,8], falha
  no sufixo selecionou [8,16], omissão de comando foi rejeitada.
  Estes são testes sintéticos de capacidade, não resultados do gameplay.
  A fórmula HZB passou os testes de redução e rejeição de operandos
  inválidos; o executor ISA integral rejeitou image_store em 0x80 por
  operand value não suportado. Não declarar essa execução integral aprovada.
- Escopo da aquisição: reparar retenção e associação reais para novos
  comparadores e preservar os certificados anteriores. O adaptador
  completo das dependências HZB/tesselação/G-buffer/HDR/cena/flip ainda
  não está fechado. A associação de pacotes não equivale à execução
  independente do prefixo. Não afirmar que a próxima captura garante uma
  decisão global. P8 permanece [0,16], sem poda global. Se surgir
  contraexemplo local, exigir efeito causal até o flip para poda global.


### #756 — APK pareado, compilado, assinado e verificado (27/11/2026)

- Revisão nativa `f6d40307f6087e53de9a1d67efca5d710356500b`, workflow [46026444215](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/46026444215), conclusão success, artefato [15211219219](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/46026444215/artifacts/15211219219).
- #755 falhou antes da compilação por regressão do filtro do compositor em
  demons_indirect_witness_test: HdrOwner passou indevidamente como overlay.
  A #756 restringe o filtro aos dois owners observados (SceneOwner e
  OverlayOwner), preserva a asserção negativa original e passa os testes
  focados, comparadores P8, compilação nativa, Gradle e auditoria do APK.
- APK ARM64 de 18.519.166 bytes, SHA-256
  `83acaf8a9d520a584572eae4c75cc194df357260b629b39e162fb27cd4a76403`.
  ZIP SHA-256 `db32735df074796b224e5991860221b267227d3a569384841de60a88e0d1ca1c`.
  Download, CRC dos ZIPs, assinatura APK v2 RSA/SHA-256 e digest de conteúdo
  verificados localmente. Certificado estável SHA-256
  `8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0`;
  revisão integral encontrada em lib/arm64-v8a/libmain.so.
- Validação adicional host: fixture de arquivo pareado com os binários guest
  reais da #753 e dados sintéticos aceitou MATCH HZB e MATCH_EXACT
  compositor; detectou divergências introduzidas de cada saída, e rejeitou
  versão incorreta do flip e versão instável da HZB. O recorte ISA HZB
  min/load com export diagnóstico substituindo apenas o image_store
  reproduziu 64/64 valores; o store integral continua sem certificação.
  Os casos sintéticos do decisor bilateral selecionaram [0,8] e [8,16].
- **Cortes globais: zero. Intervalo [0,16], P8 aberto.** Última captura real
  disponível: #753 frame 359. Nenhuma execução da #756 no aparelho foi
  observada nesta sessão. A nova aquisição repara os dados faltantes das
  comparações HZB e compositor e associa usos a pacotes guest; não fornece
  ainda um adaptador completo do cone causal HZB/tesselação/G-buffer/HDR/cena/flip.
  O comparador local de arquivo não autoriza uma poda global por si só.
  Não registrar resolução visual, P4 ou P12 antecipadamente. Próxima decisão
  deve utilizar os novos pares com versões estáveis, e estabelecer o efeito
  causal da divergência até o mesmo flip ou certificar o prefixo relevante
  completo antes de podar. Nenhuma mudança artificial de iluminação.


### #756 → preparação do novo método (27/11/2026)

**Sem novo corte global e sem APK #757 qualificado.** P(8) permanece [0,16]. O replay
do consumidor final confirmou 64/64 amostras e o quad central do frame 427;
o prefixo não está certificado. O host agora executa a ISA original de
a5fc1658592e3cba (10 leituras, quatro pixels) e b4e1b94988dff9ae (leituras
de array, profundidade e volume, MRT0/MRT1) em controles de entradas sintéticas.
A mudança sintética da profundidade de 0.5 para 0.75 alterou a saída do
segundo shader e, em substituição controlada no consumidor real de #756,
a palavra final mudou de 0xd364d936 para 0xd8962589. Igualdade de entrada
e rejeição de recurso ausente também foram exercitadas. Isso testa a
propagação, não comprova uma divergência do jogo. O raster local do céu
(draws 346/783) continuou consistente no mesmo ZIP. Não inferir erro da
HZB zerada: o depth do G-buffer é outro recurso.

As versões observadas de P8_USE_242/243 são os produtores da cena consumida
por P8_USE_418. A aquisição nativa candidata f39727ee569315463184f376ba2a1876d66f7ec3
retém operandos, índices, instâncias, estados e subrecursos selecionados
desses dois draws, com orçamento separado. Ainda não foi compilada nem
promovida à branch de APK. O replay real da cobertura/PS do segundo draw
e a autoridade das operações anteriores, incluindo a ponte G-buffer/HDR,
não estão fechados. Não equiparar os testes sintéticos a um PASS/FAIL
global, nem liberar outro APK exclusivamente por ter ampliado a captura.

### Additional offline reference on #763 (host only)
Original q33 replay with the actual scalar operands, prefix/tile buffers and same-version stencil/depth/G-buffer bytes reached 0x508 and 0x5a4. Screen UV reference dark (0.5021874905,0.6905555725), contrast (0.5028125048,0.6927778125); both first 3D lookup coordinates (16,0,3). Varying the unretained screen sample across zero, one and mixed values leaves those lookup coordinates unchanged in this test; this is a reference prediction, not a native q33 comparison. At 0x4c0 the scalar load spans two unretained padding dwords (s11/s15, overwritten before use by 0x84c). Original ISA and zero/nonzero poison controls confirmed identical prefix/lookup results. Host comparator fills ONLY those exact dead padding words when absent after fingerprint check; missing live data still fails. No native source change and no additional APK required. The actual q33 operations remain the sole active contract.

### Additional offline controls before #765 delivery (host only)

- Actual #764 q33 FP16 output is exactly RTZ; the atlas's 96/96 sampled RGB fields also match RTZ (RNE only 48/96). The q33 auditor now distinguishes complete RNE and RTZ policies, rather than treating an explained storage conversion as a shader divergence. This does not assert that arbitrary guest rounding modes are equivalent.
- On real f503 probe4957, 252/256 dominant D terms are inside the bounds of gamma-decoded albedo times min/max sampled atlas RGB divided by pi. Rays148/149/150/166 exceed that conditional bound, so do not attribute D entirely to recursive atlas: earlier direct contribution also enters the actual material join. #765 explicitly captures v20/v21/v8 separately at 0x17bc.
- Legacy SPIR-V trace f503 recorded 1536 atlas and 1536 shadow visits; 76 atlas entries on 19 rays have nonintegral layers (e.g. 1.4503871202468872), often repeated identical out-of-range coordinates. Four of those rays have nonzero albedo, only three have nonzero D. This is not a certified original instruction mismatch: old instrumentation observes host image operations regardless of guest EXEC (possible inactive/speculative lanes). The original v45 at 0x16e4 is v_cvt_f32_u32 and must be integral on a guest-active lane. #765 records only actual guest EXEC-active PCs, the loaded packed index, and both actual layer values. Host comparator checks this invariant separately even if footprint acquisition rejects the coordinate. If active invalid layer persists, reduce producer 0x16e4 and EXEC restoration; if absent, do not mistake old speculative accesses for live samples. No native sampling or brightness changes.
- Original relight ISA controls ran on 64 historical atlas/shadow and sky samples with explicitly synthetic weights and raw albedo. All five slices reproduced the separately evaluated arithmetic; zero emission and nonzero emission controls passed, and deliberate altered output detected. These controls validate the replay plumbing, not actual #765 device results.


### Offline verification before delivery (host-only)
- Actual #765 f718/f742: legacy final four-term sum is bitwise exact in all 1536 RGB fields. Same 19 bad-coordinate rays in both frames. 30 distinct historical atlas indices from both captures, plus six explicit boundary/poison indices, exercised in the original 0x16a4..0x16e8 guest ISA. 72/72 integer-layer extraction and float conversion controls passed (shift 8 and 40, mask 511), matching the independent expression. This verifies the reference, not native active execution.
- 64 retained same-position site12 interior atlas samples across the two actual f718/f742 traces: all 64 are exact FP16 promoted to FP32; 62/64 match central RNE, 64/64 match RNE of the bilinear subtexel interval assuming eight coordinate bits, versus 42/64 in the unquantized FP32 interval. Maximum absolute difference 4.164e-8. This establishes a conditional device sampling precision policy, not a new causal elimination. Host-only comparator now checks exact half representation inside quantized bounds for RGBA16F; shadow tests retain the original comparison. Added equality, mutation, non-half and wrong-resource controls. No new APK necessary.
