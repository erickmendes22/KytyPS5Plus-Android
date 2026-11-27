# #763 — contrato de iluminação indireta q33 (substitui #762)

> **Diretriz vigente — confirmação explícita do usuário, 27/11/2026:** P8/P(8) foi descartado como modelo de investigação. O único método operacional é V4, por contratos causais locais. Não há obrigação de decidir uma divisão global nem reportar seu intervalo ou status. A documentação histórica abaixo conserva evidências e decisões anteriores, mas seus roteiros, prioridades e estados globais antigos não são instruções vigentes. Os identificadores P8_USE são apenas marcadores técnicos de captura.

## Estado operacional atual — após a #761, APK #763 entregue

| Contrato causal | Estado e escopo |
|---|---|
| Aritmética q8 | MATCH condicional aos operandos observados nas testemunhas; não certifica amostragem independente. |
| Geração da máscara de clusters | MATCH no dispatch capturado: 10.982 clusters, 87.856 dwords da máscara e 10.982 resumos. |
| Transferência da máscara | MATCH: 351.424 bytes idênticos entre produtor e consumidor. |
| Seleção dos probes e leitura da radiância indireta q33 | **Único contrato ativo**. A #763 retém os operandos, texels, descritores e HDR das mesmas testemunhas. Captura de aparelho pendente. |

Próxima decisão: comparar as operações da q33 com referência independente;
se houver divergência demonstrada, corrigir a primeira e medir seu efeito.
Se houver igualdade, seguir o produtor do operando relevante à radiância
baixa. Se a aquisição falhar, reparar a lacuna concreta. Não reabrir os
contratos fechados sem evidência incompatível. Não confundir uma operação
validada com a resolução visual completa. Esta alteração é exclusivamente
metodológica, não requer outro APK.

## Registros históricos e evidências preservadas

## #761 real / #762 — indirect radiance q33, same pixel pair (27/11/2026)

- ZIP 20261127-201555 SHA256 1cbb6841b8029b53a87da6c68387bc08a18e90f3f56e458931f3e0baac54accd, core 1cc3209, 39/39 manifest entries verified. Writer shader 1ce1a858f62b5a11 frame353 submit10796 P8_USE_10 serial256096, mask guest 0x20ae6ae70 native11573980101893681772 offset175728, 351424 bytes. Reader frame354 submit10815 P8_USE_94, identical entire mask SHA256 59f339261f58ecc705ddcb9b94e41479e3a05a6774959228be0af1b3c9328b1a, no changed words, same native owner.
- Independently execute original producer ISA SHA256 6c2e72dfd79a528d48633244ce9387fa460fe27430c01632bb2643d53cca9f0e, exact scalar and four BEFORE buffers, 255 original wave64 workgroups over 34x19x17 grid. 10982/10982 masks (87856/87856 dwords) and 10982/10982 summaries bit exact. No injection of observed output. Both cluster generation and publication locally MATCH; do not reopen without evidence. Reproducible host adapter v4_cluster_isa.py uses captured dimensions and output census.
- Same q8 tile13700 lanes48/56 pixels (800,550)/(800,551), stencil86, zero incoming HDR. Corrected reference matches original cluster indices23792/23793, sample addresses and conditional RGB (max1 ULP), BRDF 474/474 within1 ULP. Only light0 contributes ~6.6e-5/~8.1e-5. Lights1/2 outside radius and light3 category rejected. Relative control, not healthy. Native samples still conditional inputs.
- Q0 excludes tile13700; q33 P8_USE_120 includes it (workgroup12196), but its HDR contribution is not representable at either witness: identical half RGB before/after q33. q33 has its own 3D lookup, probe buffers and atlas/shadow; not the q8 clustered mask. Prior arithmetic/atlas sample findings on other frames do not certify these witnesses. Active contract: the q33 lookup/load/sample/accumulator chain, from exact same pixels, actual descriptors and versions, not new assumptions about direct light masks.
- #762 acquires 23 original guest PC sites including 3D lookup 0x5a4/0x600, records 0x618/0x708/0xb44, both dynamic 8-corner loops and actual image_store 0x1174. Original q8 pair is transferred to actual q33 queue tile by unique membership, not picked independently. Six pre-dispatch buffer inputs (18MiB bounded), scalar operands, exact per-site coordinates/descriptors/samplers, wave masks and native HDR two-pixel before/after. Post-dispatch footprint readback restricted to same read-only native image identity/version and non-aliasing outputs; records 3D texel bytes, interior screen/atlas/shadow 2x2, actual sampler metadata and atlas writer candidates. Unsupported cases explicitly INCOMPLETE. Original reference is conditional on actual sampled bytes; independent footprint/record comparison and arithmetic replay are separate. No visual fix claimed. Global P8 remains historical BLOCKED [0,16]; two newly closed local producer edges, no global cut.


Run `python3 docs/android/tools/v4_gi_contract.py DIAGNOSTICS.zip --runner /path/to/kyty_p8_guest_ref`.

Pre-delivery review: DescriptorValue has an eight-dword storage container but samplers have dword_count=4. Bind only the four actually used words and reject missing sampler; use the same captured G-buffer/depth/stencil snapshots for q33 checks. The corrected native revision supersedes the initial #762 acquisition; do not request a device test of the uncorrected binary.
