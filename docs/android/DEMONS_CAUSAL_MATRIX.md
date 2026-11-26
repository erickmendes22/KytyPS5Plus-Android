# PPSA01341 01.007.000 — causal matrix
References: Jetsku dd9f990 (#130 documents fog-transition crash), Chenxiao07
66cdb39 (v20261016 family reports gameplay, no identical GPU capture).

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

Entrega técnica atual: **#746 / `4e321c2`**, APK assinado com a identidade estável,
versionCode 846, compilação e auditoria concluídas; aguarda captura do aparelho.
[Registro de verificação](DEMONS_APK_746_VERIFICATION.md).
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
