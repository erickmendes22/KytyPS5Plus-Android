# V4 — contraexemplo local e recuperação mensurável (27/11/2026)

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

## Decisão

O usuário autorizou expressamente substituir o método se não for possível
certificar o prefixo bilateral de P8. A captura #756 não permite isso:
743 ocorrências de predecessores não resolvidos, 1136 ocorrências de
bindings de buffer sem contrato de versão/acesso e nenhuma referência
independente de todos os roots relevantes. O commit histórico 838d12c8
é referência de rosto visível, não de iluminação saudável. Aumentar o
replay global não é uma estratégia de redução garantida.

**O modelo P8 foi descartado integralmente.** V4 não depende de uma divisão
ou classificação global; localizar a operação por suas dependências reais.
Não se exige certificar todos os ancestrais para corrigir uma violação
local reproduzível.
Preservar todos os certificados anteriores no seu escopo. AGENTS.md
continua obrigatório, inclusive entrega de APK quando a tarefa for de
implementação/compilação, não encerrar em progresso e não emitir build
sem objetivo verificável. A presente alteração é de método/host.

## Resultados reais, não controles sintéticos

ZIP 20261127-130905 (#756, f6d4030), frame427, submit12635, modo F.
SHA256 5939751db8dba1970af22ed1451f67df442f6f5ffde66e2fda4e24d6dee3b7cf.
Reprodução:
```
python3 docs/android/tools/p8_observable_split.py DIAGNOSTICS.zip /path/to/kyty_p8_guest_ref --draw 783
```
Runner independente: artifact p8-independent-guest-isa, run 46012579033.
Requer NumPy, LLVM gfx1010 e xxhash. O relatório verifica bytes, versões,
identidades, ordem, vínculo com o draw, listas de tiles e compositor.

* 41/41 capturas POS0: 83.190 invocações, 332.760 componentes com igualdade
  numérica (319.040 bitwise, 13.720 diferenças apenas no payload de NaN).
  Nenhuma divergência numérica nem draw unsupported. O erro do comparador
  no draw634 era exigir o mesmo offset nativo de dois descritores guest
  duplicados com 14.196 bytes completos e idênticos. Corrigido apenas no
  oráculo de computação; não é fix de renderer nem certificado de alias.
* 20 pares prepass/material têm mesmos índices e XYZW/clip bitwise.
  O material após draw783, v48781 ordem198797, tem 2825/1440000 palavras
  com RGB baixo de 24 bits não zero (proxy de material, não radiância).
  Os 1437175 pixels restantes coincidem EXATAMENTE com a máscara de
  alterações de depth do prepass do céu; zero discrepâncias de máscara.
  O céu e o cálculo POS0 comparado deixam de ser prioridade. Isso não
  comprova visibilidade esperada, nem elimina a origem dos inputs.
* Os consumidores q0/q8/q33 leem os mesmos bytes da v48781, com
  listas de tiles vinculadas aos respectivos dispatches observados.

| Checkpoint | Versão HDR | Pixels de material nos tiles | Média luminância sobre 2825 | >=0.006 |
|---|---:|---:|---:|---:|
| q0 | 9727 | 99 (3 tiles) | 0.0000147641 | 0 |
| q8 | 9743 | 2726 (69 tiles) | 0.0002150015 | 3 |
| q33 | 9791 | 2825 (72 tiles) | 0.0002150656 | 3 |
| entrada pós | 9821 | n/a | 0.0002218574 | 3 |

0.006 é apenas limiar descritivo de triagem, não valor guest esperado.
Em q8, o bit 64 do stencil original está ativo em todos os 2825 pixels
de material. A ISA faz image_load dmask:2 no PC 0x118 e AND 64 em 0x124;
isso descarta ausência desse bit nos bytes capturados, não a tradução
do load ou cobertura de invocação.

## Testemunha ativa e redução de escopo

A anterior (960,540 no flip) tem zero interseção espacial entre os 16
texels da cena amostrados e os pixels com material. Não explica o
escurecimento de uma superfície observada. Nova testemunha determinística
(799,553), palavra material 0xf4d1cfd0, tile 13899 (x99,y69), pixel local
(7,1), q8 workgroup0, q33 workgroup12139, ausente de q0. Stencil=86.
HDR observado nessa coordenada:
* q0: RGB=(0,0,0)
* q8 e q33: RGB=(0.00006628036,0.00006556511,0.00006693602)
* entrada pós: RGB=(0.00009262562,0.00008851290,0.00008255243)

O flip espacialmente correspondente (959,664) reproduz exatamente
0xc0b00400 via ISA independente; 4 dos seus 16 texels de cena têm
sobreposição espacial com a máscara material. Isso NÃO estabelece a
dependência HDR->cena (a ponte ainda aberta), apenas escolhe região
diagnóstica relevante. Não reabrir compositor por falta de autoridade
do prefixo. A baixa radiância já existe na região material antes do pós.

## Roteiro histórico da q8 — substituído pelo contrato ativo acima

Um contrato ativo por vez. Começar na fila 8 (P8_USE_93, packet7738,
CS 3dfe402653925fbf, workgroup 0, local 7,1). O alvo é a operação do
próprio shader, com a saída RGBA16 na mesma versão, não aumentar a luz
artificialmente. A q8 abrange 96,5% dos pixels materiais retidos; q33
não alterou a testemunha. Não atribuir todo o incremento q0->q8 a uma
única escrita: existem 8 dispatches nesse intervalo.

1. **Admissão.** Comparar mapeamento do tile em 0x44/0x80, coordenadas,
   bounds/EXEC e load/AND do stencil em 0x118/0x124. Os dados nativos
   já mostram tile válido e bit ativo. A igualdade só fecha a
   admissão observada, não toda a geração de tiles.
2. **Comparação local com os mesmos operandos.** Reter ISA original,
   SGPR/MODE/EXEC, argumentos consumidos, os quatro buffers efetivos
   (1.024, 1.036.800, 86.016, 351.424 bytes), SRT/regiões escalares,
   e leituras dinâmicas no workgroup da testemunha. Preservar a ordem
   e os valores das leituras e o HDR antes/depois da escrita 0x1d9c.
   Incluir versões/descritores dos 4 layers G-buffer e depth/stencil
   já suportados; para arrays, cubemap e shadow maps capturar os
   subrecursos/texels realmente acessados, coordenadas, LOD, gradientes
   e Dref, não todos os arrays. Uma trilha nativa de valores de
   amostragem permite comparar a aritmética guest desde as mesmas
   leituras, mas não é um oráculo independente de amostragem.
   Exercitar diferença de aritmética, de leitura, máscara EXEC e
   ausência de bytes. Se a execução completa do CS for inviável,
   dividir no primeiro slice de saída com valores observados e
   limites de dependência explícitos, ou portar um caminho de
   referência da mesma operação com os mesmos dados (não uma captura
   PC de outro frame).
3. **DIVERGE:** reduzir ao primeiro load, operação, branch, sample
   ou store divergente. Corrigir só a implementação demonstrada,
   testar igualdade no caso real e controle, medir contribuição
   no HDR e no sintoma. Não aguardar PASS global.
4. **MATCH:** remover a operação comparada no escopo e seguir a
   entrada que determina a contribuição baixa (por exemplo, a
   lista/coeficiente de luz ou a amostra shadow efetivamente usada).
   A referência não pode reutilizar a saída observada como resposta
   esperada. Se o resultado baixo for correto dados os operandos,
   a próxima comparação é o produtor específico desse operando.
   Uma diferença de sample exige confronto com texels/descritor e
   contrato guest, não uma substituição arbitrária.
5. **INCOMPLETE:** não compilar novamente só para recolher outro
   inventário; resolver a aquisição e o comparador offline ou mudar
   para uma referência local executável. Uma captura por contrato
   preparado. Antes de emitir APK, demonstrar controles de igualdade,
   divergência, associação de versão, ausência/truncamento, e que
   os desfechos apontam ação concreta. Não transformar uma aquisição incompleta em obrigação de resolver
   outro modelo de investigação.

A baixa população de material é uma dimensão de verificação separada,
não uma segunda hipótese simultânea nem prova de duas causas. Depois
da correção da contribuição ativa, medir a população no mesmo save e
câmera. Se persistir déficit observado, seguir a geometria não admitida/
os produtores de posições e contagens; não repetir os 41 VS ou o céu.
Nenhuma restauração de luz/projeção/depth/contagem artificial. Validar
a correção em gameplay com a mesma configuração e controle positivo.

## Entrega e limites

O programa de auditoria V4 e o ajuste de oráculo foram executados
offline no ZIP real. Não houve correção de produção, nem APK #757
qualificado nesta etapa metodológica. O contrato de execução de q8
ainda precisa do comparador e captura de operandos descritos; este
documento não declara que o shader é culpado nem que já pode decidir.
A continuação com APK deve implementar esse contrato, testar e
entregar o binário assinado conforme AGENTS.md, sem exigir certificado
global impossível e sem emitir outra build de inventário.

### Additional offline reference on #763 (host only)
Original q33 replay with the actual scalar operands, prefix/tile buffers and same-version stencil/depth/G-buffer bytes reached 0x508 and 0x5a4. Screen UV reference dark (0.5021874905,0.6905555725), contrast (0.5028125048,0.6927778125); both first 3D lookup coordinates (16,0,3). Varying the unretained screen sample across zero, one and mixed values leaves those lookup coordinates unchanged in this test; this is a reference prediction, not a native q33 comparison. At 0x4c0 the scalar load spans two unretained padding dwords (s11/s15, overwritten before use by 0x84c). Original ISA and zero/nonzero poison controls confirmed identical prefix/lookup results. Host comparator fills ONLY those exact dead padding words when absent after fingerprint check; missing live data still fails. No native source change and no additional APK required. The actual q33 operations remain the sole active contract.
