# PPSA01341 01.007.000 — causal matrix

> **Diretriz vigente — confirmação explícita do usuário, 27/11/2026:** P8/P(8) foi descartado como modelo de investigação. O único método operacional é V4, por contratos causais locais. Não há obrigação de decidir uma divisão global nem reportar seu intervalo ou status. A documentação histórica abaixo conserva evidências e decisões anteriores, mas seus roteiros, prioridades e estados globais antigos não são instruções vigentes. Os identificadores P8_USE são apenas marcadores técnicos de captura.

## Estado operacional atual — captura #763

Único contrato ativo: q33, aquisição incompleta na #763. Reparar o filtro de lançamento e os comparadores de formato/sampler; não ampliar o ramo. Geração e transferência das máscaras permanecem MATCH no escopo já demonstrado; aritmética q8 permanece MATCH condicional. Nenhum corte causal novo nesta captura. Ver [relatório de reparo](DEMONS_APK_764_V4_GI_ACQUISITION.md).

## #763 real / next APK — repair q33 acquisition (27/11/2026)

- ZIP 20261127-204832 SHA256 27461144f3b447593f1e03b0ba035220289e90f128191f72a6e26bc86a735849. All 39 manifest files verified, 127404926 bytes. Frame504 submit15160, tile15500, q8 WG45 lanes43/60 pixels (803,621)/(804,623), same stencil86. This time there is actual incoming contrast: dark L=0, relative control L=0.070633544921875 before q8; the control is not certified healthy.
- q8 original guest replay on both witnesses: 20 sample coordinates/descriptors/samplers exact, final RGB max2 ULP, six BRDF slices within1 ULP, cluster index29232 exact. This is revalidation, not a new elimination. q33 adds one half-float ULP per dark RGB channel, from (2.5987625e-5,2.6643276e-5,2.4676323e-5) to (2.6047230e-5,2.6702881e-5,2.4735928e-5); control HDR unchanged. On 1913 material pixels, mean L after q8 0.0002870207993, after q33 0.0002870862174; 932 changed. Small q33 effect alone is not a guest divergence.
- **No new causal contract closed by this capture.** q33 event v4_gi_incomplete, reason unsupported_launch, groups [22600,1,1]. q33 dispatch itself did execute and tile15500 has unique membership at workgroup17516. Six actual buffer ranges total 14.239746 MiB, all read-only. Scalar operands complete. The old guard copied the q8 requirement group_id[1] into q33, although original q33 only uses incoming s2 group X (0x64); it initializes/overwrites s3 before use. The old single failure code did not retain the individual launch fields, so the precise rejected flag cannot be recovered retrospectively.
- Offline original ISA q33 prefix through 0xe4 on the real #763 scalar operands and same-bound q8 prefix/tile buffers (identical native ranges, no intervening overlapping bound writers): 64/64 coordinates exactly match tile15500, all lanes admitted. Repeated with unused SGPRs poisoned 0,0x12345678,0xffffffff: identical. This proves that Y group ID is unnecessary to the guest prefix, not yet a native q33 instruction match.
- Repair: q33-specific launch guard accepts X-only workgroup ID, preserves real 8x8 wave64, unpacked groups and required local XY. Retain every launch field and distinguish invalid dispatch/ABI/trace-buffer conditions on failure. Add independent original full-wave prefix reference. Exact observed slot5 descriptor [49283072,65539,262144,20484] is stride1 k8UInt; 0xb44 returns zero-extended byte, not a dword. Test actual descriptor, last byte and OOB. Actual sampler0 [0x8060400,0xfff000,0xaf00000,0x40000000] AnisoLinear; q33's fingerprinted explicit LZ sites have zero derivatives and an isotropic point footprint (Vulkan Sampling, Derivative Image Operations and Scale Factor Operation), so support the equivalent linear filtering at level0 with same view. Test black/white footprint and unsupported LOD. All prior capture layout, budgets and identity guards retained. Do not classify missing data as guest divergence.
- Next remains the same local q33 lookup/load/sample/HDR contract. If actual operations diverge, reduce and correct first demonstrated difference; if equal, follow the particular input that determines the low contribution, considering the real incoming contrast. If acquisition fails, use its precise new reason, not a new hypothesis. This APK repairs the diagnostic; no rendering change or visual fix claimed.


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


## Prioridade V4 (27/11/2026)

A bisseção global P8 permanece sem certificado; não constitui veto à
correção de um contraexemplo local. M5/M7: primeiro contrato ativo
q8 tile13899, CS 3dfe402653925fbf, WG0 local7,1, material v48781,
HDR após q8 v9743. M1–M4 conservam os certificados no escopo:
41/41 VS com igualdade numérica, máscara do céu complementar ao
material, bit64 presente em todos os pixels materiais. População
material e contribuição de radiância são medidas separadas, não
hipóteses simultâneas. Ver [V4](DEMONS_V4_LOCAL_CONTRACT.md).
Sem alteração de produção ou APK nessa etapa.

References: Jetsku dd9f990 (#130 documents fog-transition crash), Chenxiao07
66cdb39 (v20261016 family reports gameplay, no identical GPU capture).

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


## Continuidade executável após #745 — 26/11/2026

[Árvore atualizada: estado global, cortes locais e decisão da #746](DEMONS_PRELIGHT_BINARY_TREE.md#estado-atual).
O nó global continua sendo **P(8), intervalo [0,16], sem metade eliminada**;
os certificados locais da #745 permanecem válidos no seu escopo. A comparação
CPU da #746 aguarda captura e não seleciona antecipadamente um ramo global.

**Prioridade corrigida: decidir P(8).** A #746 valida READ/EMIT, mas MATCH
não certifica C1..C8 e DIVERGENCE ainda precisa demonstrar efeito no sintoma
do mesmo episódio. O auditor `tools/prelight_v3_gate.py` mantém P8 BLOCKED
sem caminho executável para PASS/FAIL; integridade do ZIP não supre esse
comparador. A próxima build diagnóstica deve ter aquisição e verificador
capazes de decidir os dois lados de P(8), usando o cone necessário da
testemunha e as provas já obtidas. Isso não exige a resposta antes da captura
nem um oráculo externo de PC. Ver a prioridade operacional no início da árvore.

[Balanço obrigatório de eliminação por build](DEMONS_BUILD_ELIMINATION_LEDGER.md):
registrar cortes efetivos, revalidações, motivo de nenhum corte e a correção
concreta da deficiência antes da aquisição seguinte.

Entrega histórica: **#746 / `4e321c2`**, APK assinado com a identidade estável,
versionCode 846, compilação e auditoria concluídas. Diagnóstico real
`20261126-144336` já analisado; #747 (`63ea76b`) foi candidato de replay
nativo, não de decisão P8. Staging atual registrado no balanço de eliminação.
[Registro de verificação da #746](DEMONS_APK_746_VERIFICATION.md).
Gameplay #745 registrado no ZIP `20261126-132544`, modo F.
[Resultado completo](DEMONS_APK_745_RESULTS.md): 375/375 arquivos obrigatórios,
zero buffers declarados ausentes, 45/45 POS0 comparáveis iguais bit a bit
(73.580 vértices / 294.320 componentes). A máscara de perda pelo céu também
foi reproduzida com os operandos novos: 1.439.342 pixels e 658 preservados.

Próxima fronteira da mesma cadeia: batch CPU `0x21de00078`, três records de
20 bytes. Comando 54823 usa 98.304 índices no primeiro record; 55986/56594
usam zero, com uma instância. O fallback existia nos dois extremos e não é
por si só defeito. Não há fonte nativa/versão desses 60 bytes no ZIP.
Comparar bytes realmente consumidos com a versão independente antes de
investigar o produtor do zero; preservar ambos extremos e as transferências
observadas. Referência, leitura e emissão coincidentes podam essa aresta local.
Leitura divergente de bytes GPU autoritativos com identidade/ordem suficientes
fixa publicação/transferência/leitura; referência e leitura iguais com tuple
errado fixam decodificação/emissão. Diferença tardia em página CPU-dirty é
UNSTABLE. Não usar um espelho Vulkan CPU-dirty como autoridade nem um fault
como último store.
[Aquisição, orçamento e controles de decisão](DEMONS_CP_INDIRECT_CAPTURE.md).

Mesmo caso e ciclos correspondentes: #741 40367→40979; #744 39624→40091;
#745 55824→56618, recalculado com os operandos dessa execução.
A autorização de aquisição não depende de já conhecer a resposta global.
Foi removido do auditor o veto constante à compilação, preservando os estados
de evidência: ainda não há uma divisão global P8 demonstrada nem uma correção
de iluminação comprovada.

### Aquisição implementada na #745 e verificada com o diagnóstico novo

| Aresta | Lacuna anterior | Resultado da #745 |
| --- | --- | --- |
| Clear executado → retenção do primeiro input | O seletor anterior perdia o primeiro input após o clear. | Ciclo completo com 50 inputs, incluindo o primeiro após o clear executado. |
| ISA guest → POS0 nativo do céu | A #744 certificava 152 componentes; os novos operandos exigiam recálculo. | Comparação bit a bit repetida com os dados da #745, integrada aos 45 draws certificados. |
| POS0 do céu → D32/material | A máscara da #744 não pode substituir a da execução seguinte. | 1.439.342 pixels de depth alterados, complemento exato dos 658 pixels RGB não zero. |
| Operandos foreground → POS0 | Bytes/índices e vários shaders não foram exportados na #744. | 45 draws / 73.580 vértices / 294.320 componentes coincidem; os outros cinco têm classificação explícita. |
| Captura local → ZIP | Seleção e limites antigos removiam dependências. | 375/375 dependências obrigatórias; 608/608 arquivos com tamanho e SHA-256 conferidos. |

O log SRT observa o retorno dos callbacks originais, incluindo o leitor
ordinário usado pelo caminho Android; não relê a memória depois do consumo.
Somente F desativa a memoização de snapshots para preservar endereços atuais.
O caminho A mantém seu comportamento. O recorder generalizado preserva as
instruções originais e rejeita layouts incompatíveis com motivo explícito.

`prelight_vertex_oracle.py` também reproduz W0 NaN da #737 e localiza em W1
a lacuna de slot2/load0xc8. São controles positivos/de suficiência do oráculo;
não reabrem W0/W1 como hipóteses causais da testemunha atual. Uma discrepância
numérica terá seu modo de precisão e sua ligação ao sintoma conferidos antes
de se tornar fix de produção.

## Evidence ledger (9 ZIPs through 201032)
Unique captures: cb3b arithmetic 138/138, f0c140 count transfer 1344/1344,
fb18 first gate metadata 1748/1748 matched. fb18 84/107 nonempty inputs
have zero outputs, also in bright frames; not proven wrong. Two small
vertex/depth/material pairs project consistently. Dominant depth draw
executes after rejection, not the initial cause. Sampled atlas coefficient
transfer and descriptor 224 bytes match. Radiance provenance and first
loss of albedo remain open. Do not reopen excluded operations without
new evidence.

## Modes, same save / camera / resolution / driver
A control (no full dumps); B1 only index culling; B2 only sun shadow
index culling; reversible managed args, no eboot changes.
C1 PC-style coherent CPU reads of indirect count/records/pending
NUM_INSTANCES, log dirty/pending/changed/fallback; C2 attachment-write
availability barrier in GPU indirect fixup. D1 strict cross-frame order
without ahead-of-GPU semaphore; D2 finish before binding selected compute
dispatches (SRT specialization may already have run; not all queues).
E/F heavy capture: E independent CPU oracle for cb3b arithmetic; F
geometry/G-buffer/relight plus wider fb18 capture and same oracle.
Full isolated GPU replay not yet implemented; spatial/BDA operands and
unmatched variants remain explicitly unavailable. Fix unreachable fb18
probe guard; exact fingerprint validation required; export raw modules
for offline analysis. A–D visual result + session log; E/F full ZIP.
No lighting correction or gameplay verification claimed.

## Exit criteria
First mismatching producer/consumer or guest-semantic output. If an
isolation mode recovers the image, narrow to specific operation before
making default change. If negative, use E/F data to discriminate M1–M7.

## Capture completeness
Both native fb18 fingerprints 737447ebe010052e and bc2046dc2a822ad8
have identical defining instructions at all 157 recorded sites, verified
from 185122. Accept only those exact hashes and validate the instrumented
module; export unknown modules without instrumenting. Compare each captured
first-gate input from actual GPU descriptors to the guest ISA-derived gate
and the recorded lane's count, active state, object ID, flags and pass.
Missing values are unavailable, not a pass. This narrows M1 but is not a
full offline visibility replay. A–D exclude cached full diagnostic dumps.

## Build #717 / 53d8085 — mode F, ZIP 20261125-210141
Session 21:00:05 GMT-3, PPSA01341, Turnip GameHub 26.3 DD GMEM,
720p output. Capture enabled; fb18 fingerprint 737447ebe010052e assembled
and validated. Eight dispatches, 136 active object lanes: first-gate replay
136/136, BDA sampled load vs backing 272/272 (not proof of all BDA),
float comparisons 272/272, subgroup-any decisions 64/64. In all 136 lanes
site 3979 = 1.0 and the actual unordered less-than 1.0 at site 3980 is
false; site 4097 = 0 and no subsequent atomic emission reached. Same
condition in lit-reference and dark windows; no count-table changes within
any of eight observed fb18 dispatches. Do not call this a causal culling
bug before establishing the producer of the value at 3979.

M3/M6/M7 concrete inconsistency: cb3b9c1ea64fe876, frame 307 submit 9276
capture_seq 20, output guest 0x20817daa0. Descriptor slot 0 address
0x20934ae80 index 12 GPU=0 backing=0, slot 1 address 0x2093622b0 index 85
GPU=0; secondary limit 84, primary limit 30. AMDGPU ISA 0x44..0x50 and
0x70..0x90 predict [1,1,1,0,0], observed [1,1,1,0,3]. Slot 2 before GPU
[1,1,1,0,1], before CPU backing [1,1,1,0,3]; both input ranges GPU-dirty.
7/8 captured cb3b operations match. Source writer candidates 9ee22f0ba0ad42f8
and c4e786c3a7c0eadf, same frame/submit; range overlap is not proof of last
writer. Both work counts zero; not proof of scene-loss causation and occurs
after onset frame 284. Need distinguish captured vs actual shader input,
output publication and synchronization, not force DWORD 4.

Bridge (frame, G-buffer colored/64, HDR lit/64, mean): 164 64 64 .07518336;
284 0 0 0; 332 12 0 .00007316; 476 3 0 .00000971. M4/M5 remain open.
Next discriminatory existing APK mode D2 (same scene, no heavy captures):
checks selected compute completion before bindings; negative visual result
cannot rule out all coherence errors or certify cb3b replay. B1 only if D2
negative: independent original index-culling path, not fb18 object gate.
No new APK necessary for these modes, no rendering fix claimed.

## Build #717 — D2, ZIP 20261125-211345
Core 53d8085, launch 21:07:04 GMT-3, KYTY_DS_MATRIX=6, same driver,
resolution and title/version. Frame 1464 reached, exit reason 1/status 0.
128 capped DS_MATRIX_ORDER records: cb3b 64; c03a 13; e3da 13;
acce 13; 567d 13; bcbc 12. No fb18 or f0c140 in the capped records;
absence is not proof those shaders were never serialized. D2 completion
runs after SRT specialization, before binding; does not serialize all
queues. Queue 33 valid GPU dispatches match tile counts at frames
241/481/721/961/1201/1441 (22598/22600/22579/22579/22579/22579).
Five deliberately selected colored albedo samples at each of these frames
are not a measure of full-screen coverage or illumination. Last 5-second
present intervals report 40–41 completed flips. Pixel probes disabled;
no HDR or count replay in this ZIP. Visual result unknown until tester
reports it. Cannot determine whether prior frame-307 DWORD discrepancy
resolved, nor rule out coherence/ordering in general. If still dark,
proceed to B1 as planned, same scene and visual outcome with ZIP.

### D2 visual outcome confirmed by tester
Lighting exactly unchanged. Negative for the selected compute-completion intervention; does not rule out all synchronization/coherence defects. Do not repeat D2 without new evidence. Next: B1 independently disables index culling, same scene/save/driver, record visual outcome and ZIP. Existing APK #717; no rebuild requested or required.

## Build #717 — B1, ZIP 20261125-211809
Core 53d8085, KYTY_DS_MATRIX=1, same title 01.007.000 and Turnip driver/720p output. Frame 555 reached; exit reason 1/status 0. Tester reports unchanged lighting. Last indirect feedback 14336 records, 7898 nonempty, 6438 empty (cumulative sampled feedback, not all draws). Queue 33 valid at frames 241 and 481. GPU argument path and f0c140 producer remain active. Launcher selected B1, but no guest confirmation of actual convar consumption in ZIP; negative for configured intervention only, not proof that all culling is correct. Next C2, since actual GPU indirect path is present in logs and prior cb3b discrepancy concerns actual GPU/backing values. C1 CPU fallback lower priority; B2 shadow index culling lower priority without evidence linking it to albedo loss. Use same save/camera/config and record visual outcome. No new build required.

## Build #717 — C2, ZIP 20261125-212118
Core 53d8085, KYTY_DS_MATRIX=4, same driver and 720p. Tester reports no visual recovery. Reached frame 557, exit reason 1/status 0. 89 DS_INDIRECT_FRAME_COUNTS lines and 14 DS_GEOMETRY_INDIRECT_ARGS lines; queue 33 captures at frames 241 and 481. No DS_MATRIX_GPU_BARRIER marker in current session or recent HLE logs; actual intervention execution is not independently certified by this ZIP (LOGF marker may not reach exported session log). C2 negative for configured launch only; no claim that all GPU coherence is correct. No DS_LIGHT_BRIDGE/count replay files in C2 by design. Next C1: test PC-style CPU argument/pending NUM_INSTANCES read; require DS_MATRIX_CP evidence before interpreting a negative as a verified intervention. Do not keep cycling unobserved paths: if C1 absent or negative, focus next diagnostic revision on same-dispatch cb3b actual load/store operands and certified producer/consumer provenance, and on distinguishing coverage loss from lighting for the same pixels. B2 deprioritized, D1 only if evidence of cross-frame dependence warrants it.

## Build #717 — C1, ZIP 20261125-212541
Mode 3, frame 937, PID 12767 exited reason 1/status 0, no DS_MATRIX_CP in session/recent HLE logs. Tester confirms unchanged lighting. Do not treat C1 as a certified execution of the alternate read. End the mode sweep; no B2/D1 without new evidence. Fix missing marker export by using console-and-log rather than LOGF.

## Next causal revision
Exact cb3b native module fingerprints ac3603ad3ba167b7 (three descriptors) and 3d1cc19be27b4002 (two, secondary specialized). Instrument actual loaded index/limits, resolved offsets, loaded words, arithmetic, and all five executed store operands via dedicated diagnostic buffer. Validate each module before use, never modify original output. Compare snapshot oracle, operand oracle, actual stores and GPU readback separately; include backing and producer serial. Match queue 33 same-submit before/after HDR and albedo grid, with exact bound tile membership where available. A colored unlit pixel outside the actual queue does not certify a lighting failure; missing membership is explicitly unavailable. No claimed rendering fix until causality proven.


## Build #718 — F, ZIP 20261125-214449
Core 1dac99c, matrix 8. Both cb3b SPIR-V fingerprints validated; six
captured executions, all secondary_limit=0, match snapshot, operand,
arithmetic, actual stores and post-dispatch GPU readback. Prior live-
secondary mismatch not reproduced or ruled out. Give the live secondary
path a separate budget and exact current output address.
Paired queue 33 bridges: frame 160 64 colored covered pixels / 64 changed
HDR; frames 424,448,472 2/3/2 colored covered pixels / 0 changed HDR.
Unchanged HDR is not proof of skipped store or erroneous computation:
tiny increments round away in RGBA16F. Frame 280 three colored pixels
outside queue 33 (84 tiles), not evidence of missing writes.
Frames 241 and 481: all ten instrumented e3da stores execute.
Independent final RGB formula incoming+accumulated*scale*rgb_factor/pi
matches logged pre-quantization RGB to max absolute error 8.6e-11.
Thirteen atlas footprints reproduce logged bilinear sample to max
absolute error 1.43e-7 (FP16/filter rounding). Radiance already nearly
zero before accumulation/shadow weighting. No demonstrated final-store
or atlas-sampling defect. Do not reopen without new evidence.
Twelve uniquely mapped scatter rows have exact source GPU, destination
GPU and CPU backing (256 pairs each, no differing pair). Transfer
history records guest_backing_upload. Same probe ID 4701 has 256 colored
rays at frame 135 and 27 at 305, but different source addresses and no
certified same-position relation. No proof of corrupt guest input.
All 16 consumed relight rows lacked certified producer because extended
history was disabled. Enable existing full row history in F only,
bounded to existing 128 dispatches. Consumer digest/source backing/file
read/write-fault candidates identify provenance; overlapping reads and
faults are candidates, not proof of exact last guest writer.
Previous export consumed its budget in queue-state before atlas/radiance
and SPIR-V. Prioritize live-secondary and relight/atlas/ray evidence;
reserve 8 MiB for other subdirs, include inventory. No speculative
rendering change or confirmed correction.

### Additional independent replay of 214449
All ten captured scatter maps have consecutive producer_dispatches_seen 1..10.
Replaying the wrapped map address identifies one candidate source row for
all 16 consumed relight probes (strict earlier frame/submit); raw source
bytes for those exact rows were not in the old export. At position
(256,-464,-16,16), probe 14409 f232 maps to source row 2392 of dispatch 2
(f180), and probe 22449 f448 maps to row 2392 of dispatch 8 (f305).
All 256 raw ray pairs match bit-for-bit despite different IDs and buffers.
255/256 final RGB and all four RGB terms match bit-for-bit; max difference
in the remaining RGB output is 1.878e-12. Both maxima 7.0932634e-7. This
reproduces the low radiance for the same position, not a new loss of ray
data between those frames. Across eight relight captures (4096 rays),
float32 evaluation of (A+B)+(C+D) matches all exported RGB words exactly.
Do not infer that sparse baked ray records themselves are incorrect absent
an expected asset/guest writer reference. Separate material coverage
failure persists (frame 280 only 2922 passed samples in the collected
layer-0 draw records, even with dominant fullscreen material absent;
collected records span adjacent guest frame boundaries, not whole PC pass
census). No causal connection between that loss and sparse rays is proven.


## Build #719 — F, ZIP 20261125-220255: retract the false count divergence
Revision 4f12aa1. Actual AMDGPU load/limit order confirmed by 32 instrumented
dispatches: secondary limit flattened SRT[14], index SRT[15], not reversed.
Frame 503 n55: secondary[55]=81, limit 100, primary[66]=12 limit 200,
expected [16,1,1,972,81], matches actual stores and GPU bytes. Old oracle
reads secondary[100]=0. Corrected replay all 32/32; across all 12 archived
ZIPs with counts, 204/204 captures, 184/184 unique JSON digests. Original
#717 mismatch is false positive. Do not treat it as coherence evidence.
Correct sampling gate and use shared ReplayCount helper plus regression
fixture with distinct index and limit and secondary index zero.
Ten scatter dispatches: 62,375 rows (including repeated updates), every
source GPU row equals backing and destination at capture; all 16 consumer
digests match certified producer. No file read candidates; CPU write faults
are only overlapping candidates (one inside row, RIP 0x910004142). Baked
source correctness not certified, no transport defect shown. F no longer
repeats full row history; E retains it.
Four mask edges f315/339/405/503: light counts 28/38/87/29, 961 rows.
Guest acce uses ceil(light_count/32), loops two groups at a time. Active
mask bits, population count, probe ID, group indicator all match. At f503
254 rows have residual unused groups beyond the written pair, correctly
excluded by group indicator. f315 176 same-position probes lose group1
because guest supplies only 28 lights, not proof of wrong mask arithmetic.
Descriptor active/null and group1 presence are independent (f405/f503).
Do not reopen these eliminated operations without new evidence.

## Remaining discriminating path — first HDR contribution
f529 material afd9334e8581cda2, draw submit 15589, light submit 15597:
five preserved nonzero albedo samples, same image/words, HDR zero before
and after f0c482 (queue 0, 19383 groups), 3dfe4026 (queue 8, 1590 groups),
e3daeb (queue 33, 22593 groups). Queue 8 explicitly excludes these pixels;
queue 0/33 exact membership was not measured for this material trace.
At f256 HDR mean before queue 33 .03578788 after .07561704; f496,520,544
already near zero before queue 33, unchanged after, while 9 covered colored
samples exist (8 unlit). Need isolate first HDR contributor and actual
pixel tile membership, not re-investigate scatter or count arithmetic.
Next diagnostic revision collects same-dispatch before/after fixed grid
for queues 0,8,33, exact bound tile range/count, resource descriptors and
bounded readonly buffer prefixes; checks actual material pixels against
each queue, exports exact guest code and native modules for all three.
Never treat membership unavailable or no tile assignment as a failed store.
No gameplay correction claimed: no PC capture of identical inputs exists.


## Build #720 — F, ZIP 20261125-222007
Core dec6528, matrix 8. Fourteen paired bridges: f166 q0 adds RGB mean
0.03266060, q33 adds 0.03688316 (RGB arithmetic mean). f262 albedo
0/64 before q0, other layers and depth 64/64, q0 covers 44/64, q33 covers
64/64, both HDR zero. f278 q8 adds HDR at two covered colored points.
f310 depth 0/64, albedo 4/64, q8/q33 cover none. f454/f478 layer0 only
2/64 colored, other layers and depth 64/64; q33 covers all 64 and neither
colored point changes in q33. f478 q0 changes one of two covered colored
points. Do not infer missing stores solely from unchanged half floats.
First widespread loss is already in G-buffer before lighting.
Material f262 s7797: before afd933 853/1440000 colored, after 893;
e31a after 926. f454 s13756: 53726 colored before afd933, 53833 after;
full-screen 552d 58933->51119 (7814 lost), 1388873 depth-passed samples.
Depth writer aff9c7 f453 s13755 fills 113071->1440000; corresponding
fullscreen material executes. This is not proof the background is wrong.
Geometry census f262 and f453 includes zero-sample indirect material draws;
not proof of invalid counts or that these draws should be visible. Earlier
capture samples only watched PS and first instance of each PS; the first
loss/absence of expected material is not yet identified. Next revision:
mode F capture every actual R32 layer0 attachment draw in bounded adjacent
host-frame windows, before/after full-layer color counts, lost/gained,
64 exact pixels, depth grid, image owner/version, shader and index/indirect
identity, color mask, clear and bound guest pixel descriptors. Export first
in ZIP. Compare ordered producer chain, distinguish clear, black overwrite,
missing coverage and between-draw changes. Do not modify graphics semantics.

### Matched depth/material pairs, independently replayed from #720
Pair 7: f477 s14546 VS 34c51585c926a664 -> f477 s14548 VS f825a988127faeb4, same indices and 3421/3421 bit-identical clip coordinates and clip distance. Of 100 changed prepass pixels, 0 retain their depth and 100 are replaced with nearer reverse-Z values before the material draw. Pair 8: f477 s14546 VS a7208481a1f2b760 -> f478 s14548 VS 74b55dea9562eda1, same indices and 6303/6303 matching clip coordinates; 1051 of 1070 prepass-written pixels retain depth and 19 become nearer. This is evidence of intervening depth writes, not proof of incorrect occlusion or a vertex transform defect. Preserve normal depth comparison. Collect writer ordering and compare the actual contribution of the dominant and missing materials before changing rendering behavior.

## Build #721 — causal albedo chain
Revision 121bc4a3b4ab96a76728c79e09bf079096d17087, run 45735376620, artifact 15078906403. Fast required tests, ARM64 build, APK unit tests, compatible signing-certificate check and packaged native dependency audit passed. Optional full native regressions skipped. Downloaded ZIP SHA-256 e3c6f4bb57cf9f8aa6188e8b20ed92da41245385e3d4bef4069122b609eca8b0 matches GitHub. Extracted APK SHA-256 d4e0a3b0a744a98a657899921d634d70b864cfc4166d73b10d1b74fa9c27cc16; embedded full revision verified in libmain.so; independently verified v2 RSA signature and signed content digest. APK is diagnostic only, no gameplay correction proven. Mode F capture capped at 96 layer-zero attachment draws per phase over adjacent host frames; readbacks may perturb timing. Gaps between draws are evidence to investigate, not a complete census of other image operations.


## Build #721 — F, ZIP 20261125-223802
141 valid layer0 draw comparisons, no capture capacity reached. Phase0
f190-191 4 draws, 1440000 colored before and after each; phase1 f286-287
41 draws, zero colored before/after, zero changed words, all 41 original
occlusion queries zero; phase2 f334-335 73 draws, 345 gains and 3 losses;
phase3 f477 23 draws, 568 gains and 472 losses. Within each phase consecutive
color counts match. All 475 lost pixels attributed to 552d823d, too late
and too few to explain onset. At f286 s8287 and f287 s8312 all 41 original
scene-depth draws with depth write enabled preceding the material pass
have zero passed samples (20 + 21). First prepass consumes a pending HTile
load clear to 0, then fails to populate depth; zero at all 64 material grid
points. f286 PS 3dce2bd7 raster/no discard 13, keep discard 0, no cull 30;
895c9fe0 302/0/694. Both selected small meshes lose fragments, but neither
proves an incorrect discard. Dominant VS 868546d85ce5090a, PS
6254880342eb8db4 and 31e660625354d953 have never had this replay; these
carry most of the direct indices. Next F revision extends existing validated
no-attachment-write replay to the five additional measured scene-depth PS,
one attempt per shader/phase. Record raw SRT, V#/T#/S# descriptors, VS/PS,
submit/tick and explicit unavailable vs measured. Prioritize exact guest
and native shader modules in export. Compare raster, discard survival and
original depth/stencil query for same draw. Keep shader side-effect,
alpha-to-mask, depth-export and other existing safety gates. Do not force
production depth or change discard; no demonstrated semantic mismatch yet.

## Build #722 — F, ZIP 20261125-224726
Revision 5335d15. Replay measured six original scene depth draws at f311,
six at f359, seven at f502. No capture capacity reached. At f359 s10845,
PS 62548803 raster/keep-discard/original 324/67/62; 31e66062 537/0/0;
3dce2bd7 7/0/0; 3cc41a57 2/0/0; 895c9fe0 183/0/0; e1638ea4 28/0/0.
Total 1081/67/62. All six have identical live image descriptor ordinal 0
[0208a300,ca900000,800fc00f,90960fac,0,60,0,0] (hex). At f311 all six
and f502 all seven have null ordinal 0 and no discard losses in replays.
Same index address 0x25e6b5de0, 9852 indices, offsets 0, instance 1,
VS 868546d85ce5090a PS e1638ea46c70bc9c: f311 19/19/13; f359 28/0/0;
f502 990/990/0. This is the same index identity, not proof of identical
vertices/constants. Strong shared descriptor correlation, not proof of
bad resource data or incorrect guest discard. f502 proves a separate
late-test rejection. Original tests include stencil. Next isolate actual
guest branch predicate, sampled coordinates/value, guest descriptor source,
image owner/version and producer of the live ordinal-zero resource, and
compare guest ISA to native lowering. The ZIP has guest PS code but no
matching native SPIR-V for these selected hashes. Do not force null,
remove discard, or change depth/stencil without semantic evidence.


## Offline guest ISA replay of #722 and next focused capture
All six original PS binaries use a common image descriptor from a scalar
load at +0xa00, sample red, ordered float greater-than, AND-NOT with the
saved EXEC mask. For e1638ea4/62548803/31e66062 guest offsets 0x154,
0x180, 0x1ac, 0x1b0; 3dce2bd7 and 3cc41a57 compare 0x1a8;
895c9fe0 compares 0x1b0. For e1638ea4 gate at 0x44/0x11c uses scalar
load +0x28, SRT flattened[57] is 1.0 at f359, zero at f311/f502.
Descriptor address observed 0x208a30000 (low base 0x0208a300).
Other material predicates also exist; cannot equate total replay losses
with first gate without exact native data. Prior exporter prioritized
new shader hashes only AFTER the allowlist, which omitted them. Add
allowlist and exact bound module filenames (not merely guest hash); retain
native module variants and original descriptor evidence. Snapshot unique
bound native view by low-base address, format/component mapping, mip/layer,
owner/version/dirty state and bounded raw texels and guest backing.
Capture upload and GPU-write candidates at the observed resource address;
no claim of writer provenance for other allocations. Native/guest raw
bytes use potentially different tiling and formats, not directly comparable.
Replay comparison still measures total discard; actual first compare operands
not yet instrumented because prior native modules missing. Do not change
shader semantics or force the input to null. Exact native SPIR-V and bound
texture evidence will permit fingerprinted instrumentation if still needed.

### PC source cross-check (U59 dd9f990)
EmitFloatCompare, S_ANDN2_B64 dispatch, S_U64_MASK and ReadMixF32 have
matching relevant semantics. V_INTERP_MOV_F32 frontend matches; backend
per-vertex lowering and Android manual-interpolation optimization require
actual module evidence. PC optional ps_exec_words and f16_exact_round are
both false by default; no speculative backport. Frame 502 also has early
large occluder writes (VS 23371923, 7296 indices, 1094892 passed samples)
before the smaller scene-depth draws. Do not assume a single gate explains
persistent darkness; the separate depth/stencil rejection remains open.

## Build #723 delivery
Revision f41e4c10eebafdc6dab8068d6d6ff743063f91a6, run 45738716818,
artifact 15080661176. Mandatory fast tests, native compilation, APK tests,
certificate compatibility and native dependency audit passed. Full optional
regressions skipped. ZIP SHA256 f044962c146de62d509105f2fcb2798d9647bc3de3ee66e7bd41707f3f27dfe7
verified; APK SHA256 0a297246dd6286ee62a98c074af6671f6344bb4659b38195441d920fe1dceb43,
16273686 bytes. libmain.so contains exact revision; APK v2 RSA signature
and signed content digest independently verified; certificate identical to
#722. Diagnostic only; test F. Actual first compare operands not captured
in this revision; exact native module and resource bytes required before
fingerprinted instrumentation or semantic correction. Address-specific
writer log may miss a dynamically relocated allocation.


## Build #723 — F, ZIP 20261125-230251
Exact SPIR-V exported for all seven measured PS hashes. f283 500/500/387,
f331 237/237/104, f474 12003/12003/4583 raster/keep-discard/original;
all 19 selected original draws have null descriptor 0, depth and stencil
tests enabled, compare 6. No discard loss in this launch's selected
windows, despite persistent sparse albedo. #722 f359 discard is not a
necessary condition of darkness. f283 layer0 chain 1280->1479, f475-476
41239->42101 (boundary counts, not net draw-only changes). Two CPU-dirty
64 KiB uploads at guest 0x208a30000, no matching GPU-write log. No active
input snapshot in the selected windows. Native e1638ea4 57f8a300c64ea876:
SPIR-V word offset 2205 OpImageSampleImplicitLod result 304; red bits
307; bitcast float 317; threshold 316; word 2267 OpFOrdGreaterThan
318 = 317 > 316; logical NOT 319 and ballot 320. Thus first comparison
is not simply reversed. This does not establish operand correctness.
Next capture live nonzero ordinal-zero descriptor independent of adaptive
frame windows (up to two per watched shader), export exact module and
bound image evidence first. Split existing read-only replay into depth-only,
stencil-only and both tests, preserving masks and all original writes.
Compare same-draw queries, not different frames; original may differ from
read-only replay if depth writes within a draw cause self-occlusion.
No graphics semantics changed.

## Build #724 delivery
Revision d0975c557a477d203f495dfb68628cc2b0611dc6, run 45740027126, artifact 15081360854. Mandatory tests, ARM64 compilation, APK tests, certificate compatibility and packaged-native audit passed. Full optional regressions skipped. ZIP SHA-256 ada8e224365380721dac639489db9cf58786a211797ff132747d76c97934a72b verified. APK SHA-256 3fa61f946a66f86459d4bd3d402a5f4bcf17f93e300d9c13ed6e16bec8e57801 (16275618 bytes), embedded libmain.so revision matches, v2 RSA signature and signed content digest verified independently; certificate matches #723. Diagnostic mode F, no production graphics fix claimed. Active image capture remains bounded and does not itself record the actual comparison operands.


## Build #724 — F, ZIP 20261125-231956
Revision d0975c5, 19 original draws with all six replays, no capacity reached.
f260/261 raster=3972, discard=3972, stencil=3972, depth=3841, both=3841,
original=3439; f308 230/230/230/102/102/94; f451
12045/12045/12045/5093/5093/4548. Total raster 16247, depth 9036,
7211 rejected by depth; no stencil or discard loss in these samples.
f451 3dce2bd7,3cc41a57,895c9fe0,e1638ea4 have zero depth-only passes
from 107,15,1712,390 raster samples respectively. f260 3cc41a57,
895c9fe0,31e66062 have zero even with cull disabled; do not infer wrong
clipping without vertex and viewport data. f451 s13716 after HTile clear
all 1440000 depths zero; 23371923 7296 and 192 index draws report
1122222 and 484251 passing samples but zero depth changes, hence passes
alone do not prove an occluding write. Later 3dce2bd7 and 895c9fe0
see only 17846 and 20226 nonzero stored depth pixels, respectively, and
change none. s13717 aff9c7ef fills 1398973 pixels (41028->1440000 filled).
Do not suppress this draw or relax depth based on those counts.
13 event-triggered active descriptor observations f263..269 all resolve
unique native slot0, same owner 0x208a30000, version1, VkFormat133
BC1_RGBA_UNORM_BLOCK, 64x64, seven mips, guest format169, tile mode9,
pitch512, 65536 guest backing bytes, all SHA256
1b481f49d81dbbefd892d736fb9c4e89f79d8651619d314a9945d09683d2165b.
All 13 readbacks rejected by diagnostic's uncompressed-only guard.
Two CPU dirty uploads logged, no GPU write at that exact address.
Next revision captures actual compressed blocks per accessible mip with
correct block pitches, exact guest mip layout and separate mip payloads.
Add independent active-descriptor query pool phase4 lasting up to nine
host frames, so regular phase window cannot prevent sampling active
shader. Split surviving fragments by less/equal/greater against the
same original attachment with writes off; restore effective dynamic
compare and stencil masks. Record depth owner/version and sampled stored
depth grid (skip pending load clear). #724 session log records 15 vertex
captures, all omitted by 48 MiB export budget; move positions and depth
owners ahead of previously established producer captures. These are
read-only diagnostics, not a production correction or proof that the
sampled texture is corrupt. Actual per-fragment operands remain unknown.

### Offline BC1 reference and build #725 delivery
Guest 64 KiB backing from #724 reconstructed using repository Standard64KB64Tables (tile mode 9, 8 bytes per compressed block) and the 64KB mip-tail origins. Seven linear BC1 mip payload lengths: 2048,512,128,32,8,8,8 bytes; SHA-256 prefixes respectively 66b988d891574db0,d794a30e29a4ea84,d37ca62e2836eefc,3bb810dc8a85452b,f2be2576986318a9,a27c100a632942de,6a1f3577581cf300. Approximate decoded red mean mip0=0.478, range 0..1, 256 unique blocks. Pattern consistent with noise but purpose and guest layout independently unproven. Compare actual GPU block payload and recorded guest mip layout in next session; if backing changes compute a new reference.
Revision b74644e40ae01b50b4924886dba2aaaa3469a896, run 45742267999, artifact 15081344247. All mandatory checks, ARM64 compilation, APK unit tests, signing compatibility and dependency audit passed; optional full regressions skipped. ZIP SHA256 9e0970072bd6d2012906a7de59a8864e1bf09e8bf9331453cb4522645f565bfc. APK SHA256 46ef17f499c2ea0323c47082afcf206778b14c956e480b2c86e0c254df292fbb (16277142 bytes), exact embedded revision, ZIP integrity and APK v2 RSA signature plus signed content digest independently verified. Certificate matches #724. Test F; no production graphics fix claimed.


## Build #725 — F, ZIP 20261125-233709
All 12 active descriptor snapshots (f300..305) have same backing and
owner version. Independently linearized using captured guest mip offsets
and Standard64KB64 offset mapping: 84/84 mip payloads, 32928 bytes exact
match. No transfer/detiling corruption demonstrated. The reference uses
repository guest layout, not physical PS5 capture. 25 selected original
replay sets: phase1 f283 411 raster/411 survive/75 less/0 equal/336 greater;
phase2 f331 284/284/138/0/146; phase3 f474 12010/12010/6943/0/5067;
phase4 f300..305 365/271/271/0/0. All 25 less+equal+greater partition
exactly, all stencil-only equal to survive. Active draw losses 94/365
from discard (e1638ea4 83, 3dce2bd7 11), not every active draw. Other
271 survive discard but all have strictly lower reverse-Z depth.
Six same-host-frame vertex pairs (IDs3..8) have identical indices and
positions/clip-distance for every vertex (27084 vertices total; calculate
from individual row counts: 1333+6303+3421+6303+3421+6303=27084).
Pairs1/2 cross host frame 231->232 and positions differ, not same camera.
Independent CPU center-sample rasterization, two face orientations, actual
clip/W viewport and stored per-pixel depth. For the matching face at
f283 PS 3dce2bd7: 4 raster/4 pass, GPU 4/4, 2 unique changed pixels;
895c9fe0 72/25 CPU and GPU, 25 changed. f474 3dce2bd7 CPU 107/0 vs GPU
108/0, 895c9fe0 CPU 1674/0 = GPU 1674/0, zero actual changes in both.
No bit-exact hardware raster claim at boundaries, no guest vertex oracle.
Do not repeat small mesh depth/stencil/texture-transfer investigations.
Actual primary direct geometry pairs 868546d85ce5090a -> 13f07ce83dc1cc67.
At f474 22236 indices passes 2762->534; 57066 passes 29483->13556;
12765 same index guest 0x27c3a7280 f283 and f474 has zero prepass AND
material coverage. f283 and f474 background pair aff9c7ef -> c96f3fcd
27/36 indices passes exactly 1438773/1397147 in each corresponding
prepass and material draw. Background is not a mismatched depth pair
by coverage; capture positions before attributing causation. Focus next
F revision on five paired categories (6254/afd9, persistent zero 12765,
large 31e6/e31a, e163/a9cd, background), one per dark phase, exact index
identity; no repeated small captures. Native 8685 module 7843cbd43b48179b
position store verified (975,967), clip 971, vertex 75 instance77.
Other variants use validated structural discovery, no guessed transform
operands. Prioritize paired position data and exact native modules.
This distinguishes wrong positions from legitimate occlusion or missing
upstream geometry; no rendering semantic change or root cause proven.


## Fixed causal chain — user-directed methodological change
Stop rotating suspected subsystems. Witness W0: f474 submits 14134/14137,
same index address 0x27c3a7280, 25530 bytes, 12765 indices, instance 1,
base vertex/first instance 0, prepass VS868546d85ce5090a/PS31e660625354d953,
material VS13f07ce83dc1cc67/PSe31a5473e9ee7503. All 21 original records
of this index identity in #725 have zero passing samples (including three
prepass records with pending HTile clear). Not proof it should be visible.
Positive witness W1: same VS/PS pair, f474 same submits, 0x27d9906c0,
57066 indices, 29483 prepass / 13556 material samples. These counts
are not a one-to-one pixel comparison; need same pixel/triangle evidence.
W1 has a measurable contribution to scene coverage, so use it to
check that W0 is not merely legitimately off-camera. Follow same
draw identities, not another mesh when capture is inconvenient.

1. Albedo contribution: material coverage zero W0; W1 partial coverage.
   First incorrect value not yet proven.
2. Prepass: W0 zero; exact W0 fixed-test replay unavailable because
   previous selector takes first shader hash occurrence, not this draw.
   #725 small-mesh CPU oracle validates those samples only, not W0/W1.
3. VS export: #726 records exact paired index streams, clip XYZW and
   clip-distance, vertex descriptors, SRT, available buffer bytes,
   attachment regions and native module identity. Require W0/W1
   index address AND count in analysis; reject nonmatching captures.
   Full-screen draw and other pairs are context, not new suspect branches.
4. Guest calculation: native 868546d8 code at guest offset 0x40 derives
   instance/transform index from s10 low word and v8; scalar loads
   0x48..0x9c resolve descriptors and constants. Actual buffer reads
   0xa8 format_xyzw, 0xb0/0xb8/0xc0 transform data (offsets 32/16/0),
   0xc8 format_xyz, 0xd0/0xd8 format_xy; position export 0x288.
   LLVM gfx1030 fails to decode several PS5-specific instructions:
   do not claim independent guest transform verified. Use project's
   decoder and exact ISA semantics for those instructions, compare
   actual load/format conversion, transform and clip export in that order.
   If output matches, follow loads to resource producer or the actual
   depth writer at the rejected sample. If not, fix first instruction.
5. Producer: blocked until exact W0/W1 effective loads and producer
   range/version are identified. No new texture, depth, or lighting
   experiments absent an observed edge divergence.
No new production fix established. #726 already underway; evaluate it
against this chain, do not call a nonmatching capture progress.

## Árvore binária pré-iluminação — etapa 1
Por solicitação do usuário, a investigação agora usa a árvore canônica
[DEMONS_PRELIGHT_BINARY_TREE.md](DEMONS_PRELIGHT_BINARY_TREE.md): 17 nós
binários, 18 folhas, poda P01–P06 limitada aos casos já certificados.
Releitura dos ZIPs #721–725: #721 f286–287, 41 material e 42 registros
de depth-only no attachment, todos zero amostras aprovadas; #725 fase1
1154->1252, 42 draws (329 ganhos, 231 perdas), início já esparso.
N00 (albedo esperado chegou a existir antes da perda?) está bloqueado
na transição não observada. Priorizar delimitação do primeiro ponto de
perda/ausência e separar produção de retenção/publicação; não inferir
visibilidade de W0. Não há folha causal comprovada. #726 foi disparada
antes da nova árvore; somente usar dados que fecharem um nó. Nenhuma
nova build solicitada/disparada pela construção da árvore.

## Build #727 — F, ZIP 20261126-075253
See DEMONS_PRELIGHT_BINARY_TREE.md. f239 physical overwrite by
552d823d, f265 loss interval includes upload. Do not infer wrong
background or material shader. Trace exact buffer producer and
post-acquisition bytes; background SRT/depth pair. Journal reached
capacity f293; late gameplay not certified. No rendering fix yet.

## Build #729 — F ZIP 20261126-081531
Eight dirty attachment acquisitions return zero before actual material
execution; all clear candidates specify zero. Physical N00 split
points to production after clear in f249/f250; all 11/7 material draws
have a same-frame prepass match by index identity, all prepared draws
in journal have execute. Exact guest admission and indirect records
not yet certified; native coverage/clip must be joined to same command.
See DEMONS_PRELIGHT_BINARY_TREE.md for witnesses and capture policy.
Do not reopen BC1, stencil, or compare absent conflicting evidence.

## #730 F — 083233
N20 native fixup 134/134 matches. W0 12765 has 2624/2624 NaN
XYZW both VS, already 7872/7872 NaN words in input XYZ; W1 57066
finite 37358/37358 equal exports. Follow N32/L09 to producer, not
repeat raster tests. Actual expected visibility not established.
See DEMONS_PRELIGHT_BINARY_TREE.md for counts and next N70 split.


## Revisão metodológica #737/#738 — V2
A primeira queda observada após população plena no ZIP 102508 é f311 q33
(1440000, version10716, seq2729) → f312 q0 (651, version10900, seq2730),
owner 0x287ae0000 layer0 mip0. f312 não contém draw_before/draw_after.
A causa global não está demonstrada. W0/W1 são replays locais, não oráculos
absolutos nem prova de causalidade. Substituir o roteiro de hipóteses pela
bisseção de prefixo cumulativo da árvore V2, com G0 antes de [0,8],
P(4) primeiro, um intervalo ativo, BLOCKED sem descer, sem troca de ramo.
As podas P01–P06 permanecem restritas ao escopo certificado. Não prometer
três builds: são três decisões quando há evidência suficiente. #738 já
existia; aproveitar somente evidência local. Nenhuma build nova por docs.


## V3 — auditoria antes da #739
A árvore integral C1..C16 substitui a premissa pré-iluminação. P8 divide G-buffer completo de iluminação e apresentação. #738 falhou no native; substituída por aquisição circular na fronteira observada, com dados de cobertura e bridge no mesmo ciclo. Sem certificado G0/P8 ainda. Ver árvore canônica V3.


## #741 113031
First loss f233 draw40979 552d823d, 1439989->658, identical full hash to q0/q33, prepass40809 same exact index digest/1439331 samples. Prior 40780/40785 actual depth coverage high but only 1/64 nonzero stored before next draw; no proof of invalid incoming depth. PS RGB input zero, actual write consistent. G0/P8 BLOCKED pending same-cycle independent foreground/sky oracle. Preserve V3 and acquire exact fixed witness inputs and depth history; no W0/W1 detour.


## #741 offline guest replay extension
Original GFX10 occluder explicitly exports Z=0, PS552d MRT0 packs zero RGB to ff000000, and reconstructed sky projection matches 64 D32 points within 2.12e-9 under full viewport Y-flip assumption. Focus stays on actual foreground producers in first loss cycle (starts f232 draw40367 load-clear). Exact missing native inputs total ~2.08MiB. Capture all bounded actual direct prepass inputs in same matched cycle; no new tree or semantic change. P8 remains BLOCKED until expected foreground established.

## #744 / staging — auditoria reproduzível de 26/11/2026

Ver [relatório e comandos](DEMONS_V3_AUDIT_2026_11_26.md). Os ZIPs #741/#744
reproduzem o mesmo ciclo de 50 draws e âncoras completas idênticas. A seleção
pelo flag preparado era incorreta: false na captura, true no clear executado.
A staging também podia perder o primeiro payload novo quando o orçamento do
ciclo anterior estava cheio; retenção adiada até após clear/reset, com teste
de regressão da política efetivamente usada pelo renderer.

Replay local #744: zero discrepâncias na máscara inteira de alteração D32,
1.439.331 pixels alterados, erro máximo 2,375e-9 a partir do XYZW capturado.
Fórmula guest de posição VS8685 coincide em XYZW com variant7843 sob a
premissa explícita de mesmos loads/lane ativa. Fixup 43/43; os dois indiretos
do ciclo têm instanceCount=0. Não alargar esses certificados para P8.

Inventário: 3.304.608 bytes bound omitidos por usos (3.214.628 necessários
excluindo zero invocações), 269.592 índices diretos omitidos, 20/36 shaders
guest ausentes (16 necessários após excluir zero invocações), incluindo
VS557d de 15 draws de material. Filtros/caps
antigos e falta de imagens/versões/saídas impedem replay completo. Nenhum
contraexemplo causal de iluminação demonstrado. Gate executável de prontidão
reprova ambos os ZIPs e declara backend independente C1..C8 indisponível.
**G0/P8 BLOCKED; [0,16] inalterado; sem nova build Android.**

## Staging da referência independente após #747

P8 [0,16] permanece ativo. Ver [registro detalhado](DEMONS_BUILD_ELIMINATION_LEDGER.md#staging--referência-guest-independente-26112026-após-747). A calibração do produtor ea2e66ea6ad76baf reproduz 120 bytes observados no frame 215, sem certificar o prefixo completo. A matriz de câmera muda no episódio seguinte; preservar HZB e operandos da execução real, não transplantar contagens. Backend Rocjitsu em validação host, sem novo APK.

## #748 — referência AMD validada e aquisição da entrada HZB

CI 45897822805 aprovado; execução offline do shader real ea2e66ea6ad76baf da #746: 78 passos, 45 acessos, 120/120 bytes iguais. A próxima aquisição integra os bytes de imagem lidos por b213157ad251f2c7 e 7d5ad40fca3137e2 aos buffers retidos no mesmo endpoint, com identidade e digest. Não se altera a semântica. P8 [0,16], sem corte global; ver ledger.

## #748 — entrega verificada (26/11/2026)

APK compilado, assinado, baixado e verificado: revisão `0f1709763e30ab7812652d2eda81518a6e32ebff`, run [45905112907](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907), artifact [15157339392](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907/artifacts/15157339392). Registros de assinatura, integridade, ABI e alcance em [DEMONS_APK_748_VERIFICATION.md](DEMONS_APK_748_VERIFICATION.md). **Corte global: nenhum. P8 [0,16] permanece ativo; aguarda captura F do aparelho.** A entrega não fica condicionada a PC externo ou ao resultado futuro de gameplay.

## #749 — regressão de instrumentação

#748, ZIP 20261126-212515: captura Vulkan 14,2 GB e congelamento frame 380. Remover serialização global de modo F, preservar dados raw com orçamento e operandos reais já adquiridos. Sem alteração semântica de iluminação. P8 [0,16]. Ver ledger.

## #749 — entrega e replay real da #748

APK assinado e verificado em 9b67c4548dc57c90beb99a623ef80458f688cd85. Cinco dispatches de visibilidade, 712 workgroups, 960/960 bytes de contadores iguais; contraprova local dos DWORDs 0x2088806b0. Ver [balanço de eliminação](DEMONS_BUILD_ELIMINATION_LEDGER.md). P8 [0,16] permanece aberto. Captura whole-state desativada por regressão de congelamento; aguarda validação no aparelho.


## #750 — entrega verificada e seleção da fronteira escura

[Registro da entrega e evidência real da #749](DEMONS_APK_750_VERIFICATION.md). Revisão 339922af, build 45939076222, APK versionCode850, assinatura compatível verificada. P8 [0,16], sem corte global: produtores 224–225 não correspondem a consumidor 335. A #750 fixa seleção temporal e transporte; preserva os certificados anteriores sem trocar de hipótese por mera contagem. Aguarda teste F.


### #750 real capture 20261127-012846 / next build
- Selected producer frame 383 (51 draws, 41 native position records), first consumer frame 384 submit 11608, owner 0x287ae0000 version 43990 write_order 182409. 624/624 required files, continuous 1024-event journal. Previous frame-boundary retention defect closed.
- Independent POS0 reference: 41/41 observed draws MATCH, 60,076 vertices / 240,304 components, 0 divergent. Material VS13f07 conditional 0x74..0xc4 tainted; EXEC restored 0xcc; duplicate descriptors accepted only when both complete native observations have identical ranges and bytes. Two unobserved Z=0 occluders have reference positions but no host export comparison; six indirect draws have zero instances; two auxiliary draws unsupported.
- Draw 599 reduces colored layer0 2276->441; independent background prepass raster reproduces exactly 1,439,559 changed D32 pixels and preserves 441, max abs D32 error 3.875e-10. Guest PS552d exports ff000000 for captured zero RGB operands. No sky/material forced color. Deferred q0/q8/q33 HDR 3/406/433 nonzero RGB, all within the 441-pixel mask.
- Critical missing part of final-symptom witness: a direct HDR graphics pass VS00d5229e9ce98fd9 PSab81bfa8ee4e0fee, frame383 draw52 passed 1,440,000 samples (before retained depth clear119). Corresponding frame384 P8_USE_93 writes HDR after q33; input version8620, next HDR target version8622, later post readers version8628. Raw capture had no full HDR output for this pass or last HDR consumers, so prior correlation cannot certify P8. The observed pass is part of the same final-output chain, not an unrelated case.
- Next bounded acquisition: exact before/after direct HDR pass, pair linked to cut_use, actual blend/clear/write/depth state, small dynamic forward image operands, final HDR readers and actual compositor scene input. Raw caps remain 192MiB stored / 256MiB transferred. No GFXReconstruct, no blanket shader override.
- Global cut: none. Local POS0/raster matches narrow candidate operations, not entire C1..C8. Independent complete P8 expected-image backend remains absent; new acquisition addresses an actual missing final-contribution edge rather than treating a native snapshot as a guest oracle. Verify same frame, versions, and actual output; distinguish missing prepass input, dark forward output, and subsequent loss.

## #751 — captura real e aquisição #752 (27/11/2026)

P8 [0,16] continua aberto. Na captura 074347, HDR q0/q8/q33 tem 0/4/4 pixels acima de 0,006; entrada de post tem 4 e entrada do compositor 86. A escuridão antecede a apresentação. 43 draws de posição, 75.064 vértices e 300.256 componentes coincidem numericamente com a referência guest; 283.016 componentes são bitwise iguais. NaNs já existem nos operandos de entrada; 17.240 payloads NaN diferem, sem divergência numérica. Não ampliar para certificação integral de P8.

M2/M3/M6: no lote de três registros que em #750 emitiu 92.160 índices para o passe direto ab81bfa8ee4e0fee, #751 emite zero nos três filhos. O endereço dos argumentos mudou de 0x21de00078 para 0x21de07c38, fora dos quatro pins históricos. Entradas de duas execuções não são oráculo idêntico. A correção da aquisição em b167ce99 acompanha lotes do ciclo escolhido, ordena as referências com a thread de registro, preserva os cinco DWORDs realmente consumidos e os valores emitidos. C1, isoladamente, passa a drenar o filho anterior antes da leitura coerente. F não substitui valores. Sem corte global, sem fix visual confirmado.

Comparador: `python3 docs/android/tools/p8_cp_ordered.py ZIP`; teste `python3 docs/android/tools/test_p8_cp_ordered.py` (sete casos locais aprovados). MATCH/ DIVERGENCE/ INCOMPLETE/ UNSTABLE distinguem leitura/emissão e falhas reais de aquisição; não convertem MATCH local em PASS P8. A divergência de leitura só é aceita localmente com autoridade GPU dos bytes diferentes, estado estável e vínculo exato a comando/filho. Sem epoch universal CPU para endereço novo, incerteza permanece explícita. O ZIP #751 não tem o novo JSONL. A #752 deve ser testada em F, com o mesmo save e configuração; ao encontrar diferença, ligar o valor correto ao passe ausente antes de cortar a árvore global.

## #752 — análise real 082502 (27/11/2026)

P8 BLOCKED [0,16], nenhum corte global. M2: passe direto
ab81bfa8ee4e0fee presente com 828 índices (frame384 submit11367);
HDR 7717->7719, RGB idêntico em 1.440.000 pixels, alpha muda em
1.439.563. Ausência total do draw não é condição necessária. M4/M5:
440 pixels não dominantes por layer do G-buffer; os 437 pixels de
alpha inalterado pertencem a essa região. A primeira divergência
independente ainda não foi provada. M7: POS0 39/39 MATCH, revalidação
local. M3/M6: 10 registros CP, pós-backing iguais ao consumo; dois
filhos perdem autoridade GPU nos 20 bytes durante leitura, cinco
registros sem parent no frame384. Comparador reparado: 4 MATCH,
6 INCOMPLETE, 0 DIVERGENCE. Não confundir resultado posterior à
publicação com referência anterior. Lacunas de produtor, diário
pós-fronteira e referência nativa pré-leitura detalhadas no
[balanço #752](DEMONS_BUILD_ELIMINATION_LEDGER.md). Sem APK novo
ou fix visual nesta atualização offline; manter prioridade de
instrumento decisório bilateral antes da aquisição seguinte.


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
