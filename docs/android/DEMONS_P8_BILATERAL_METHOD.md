
# P8: decisão bilateral, não outra sonda — 2026-11-27

A preparação das builds #750–#754 confundiu certificados locais com a
capacidade de decidir P8. O gate anterior apenas auditava a testemunha local
de perda de cor e mantinha P8 BLOCKED. A entrega #754 não satisfaz o requisito
bilateral do AGENTS.md. Esta mudança é do método/replay host; não dispara APK.

## Resultados executados, não previstos

- p8_bilateral.py: núcleo de decisão para cone finito, com versões e operações
  explícitas, censo guest independente (obrigação do adaptador), sem flags
  fornecidas de PASS/FAIL nem pixels esperados escolhidos manualmente.
- Executa código guest original no p8_guest_ref, construído a partir de
  ROCm/rocm-systems 431f58f7841a88b705992a91d9d71fd972345beb. Contratos
  independentes implementados para cópia, clear u32, máscara de store,
  comparação D32 finita, seleção e guest wave isolada sem LDS.
- FAIL: uma diferença antes do corte, com a linha de base reproduzida e
  substituição causal propagada até a mesma amostra final, seleciona [0,8].
  Uma operação sem relação com a testemunha não precisa ser provada.
- PASS: todo o prefixo relevante da testemunha deve coincidir; uma diferença
  independente posterior, propagada ao final, seleciona [8,16].
  Igualdade só na fronteira, com erro compensado antes dela, não basta.
- Fixture RDNA1 real (v_mov/store): execução de duas variantes com um
  erro deliberado. FAIL C3 propaga comandos 10–14, PASS com erro C12 propaga
  13–14. Luminância armazenada 0 -> 0,5019607843 nos dois casos.
  18 controles de rejeição/escopo/formato passaram; comandos omitidos,
  bytes incompletos, versão obsoleta, aliases, divergência que não chega
  ao pixel, compensação e acesso guest fora da memória não produzem corte falso.
  Esses fixtures não são gameplay e não provam decisão sobre Demon's Souls.

## Evidência reutilizada da #752 e #753

A função inventory e prelight_v3_gate.py agora identificam o flip real,
inclusive em frame sustentadamente escuro sem transição bright-to-dark.
Nos dois ZIPs, 39/39 arquivos P8 íntegros, 375 usos antes do flip,
284/284 leituras da fronteira vinculadas ao payload exato, 12 versões de
subrecursos da fronteira com todas as leituras disponíveis; 288 candidatos
de operação no cone conservador (não um cone fechado). 970 vínculos de
buffer antes do flip carecem de contrato de acesso/versão; 540 ocorrências
de possíveis predecessores ainda sem produtor confirmado no cone.
A mesma contagem em duas capturas não constitui comparação da mesma entrada.
#753: frame 359, owner final 8991539200, versão 317, ordem 169712, formato 64,
P8_FULL_38.bin, 8294400 bytes, SHA256
0720eaef126f22fb30676aa1facf8fb079ebe202b2f61007f049608c29606df4.
O candidato escritor é P8_USE_375. #752: frame 384, versão final 341.
Preservar certificados CP, tesselação e POS0 já obtidos; não recapturar
as 284 leituras da fronteira já validadas para esses ZIPs.

## Contrato do adaptador real — lacunas de implementação

O núcleo bilateral funciona para grafos finitos com contratos executáveis;
o adaptador Android->grafo ainda não está implementado. O inventário não
produz certificados e não promove automaticamente bytes nativos a referência.
Não existe caminho PASS/FAIL completo para o frame real ainda.

| Entrada necessária | Origem a implementar | Verificação obrigatória |
| --- | --- | --- |
| Censo original guest, trabalho omitido/predicado/zero | pm4Dispatch/pm4Handlers e graphicsRun antes de reduzir/traduzir comandos, estado de launch e identidade pai/filho | censo independente da lista nativa, incluindo ausência de emissão; ordem/identidade da operação |
| Buffer consumido e produzido | DemonsP8Reference::InspectBindings, renderDraw/renderCompute, BufferCache; autoridade anterior à leitura, identidade/geração e intervalos de acesso, publicação depois da escrita | comparar bytes de mesma versão, registrar alias, pending e cobertura de ranges; não substituir autoridade guest por backing posterior |
| Subrecursos de imagem | renderDraw/renderCompute e cache de imagens; identidade antes/depois, formato, mip, layer, aspecto, clear/load/blend e escritos reais | conectar exatamente versão produzida e consumida, não apenas o candidato anterior |
| Contratos independentes C1..C8 | integrar oráculos existentes no adaptador; expandir referência ISA para launch multivaga/LDS/imagens e raster/interpolação/PS realmente usados | executar só operações relevantes, comparar bytes/export/cobertura, não validar com o mesmo SPIR-V |
| Propagação até o flip | versão da fronteira + contratos do consumidor e sufixo ou replay nativo limitado e verificado contra o baseline | preservar saídas reais; substituir somente referência independente; comprovar efeito no mesmo pixel/objeto |

Resolver predecessores com registro de intervalos e versões antes de
transferir payloads; retenção por conteúdo e recorte causal, não captura
whole-state (regressão de #748). Uma diferença causal de prefixo pode
decidir FAIL sem completar operações fora de sua trajetória. PASS exige
fechamento do prefixo relevante, não replay de todo o jogo. Não exigir PC,
aparelho externo, rastreador CPU universal ou resposta de gameplay antes
de entregar um APK já pronto. O requisito para nova build diagnóstica é
instrumento bilateral conectado à captura e validado offline, não um corte
obtido no aparelho antecipadamente.

## Reproduzir

    python3 docs/android/tools/test_p8_bilateral.py /path/to/kyty_p8_guest_ref
    python3 docs/android/tools/p8_bilateral.py DIAGNOSTICS.zip --output cone.json
    python3 docs/android/tools/prelight_v3_gate.py DIAGNOSTICS.zip --output audit.json

Testado em host nos dois ZIPs reais. A integração com audit é inventário
adicional de ZIP, não muda a decisão global na ausência do adaptador.
Status real: P8 BLOCKED, [0,16], 0 cortes globais. Nenhuma mudança
de renderização, nenhuma nova build, nenhuma correção visual alegada.
