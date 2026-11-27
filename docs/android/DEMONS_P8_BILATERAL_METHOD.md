
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

## Extensão gráfica do oráculo independente (host)

A execução do VS c96f3fcde8141266 no draw 599 do ZIP #753, após o prólogo NGG conhecido, com bytes e registradores da testemunha, expôs uma falha do executor: `EXP POS0` era rejeitado por falta do destino de exportação gráfica RDNA1. O backend foi alterado para registrar o callback real de exportação do GFX10, conservar target/mask/lane/componentes e aceitar LDS de interpolação explícita (P8ISA2). O protocolo P8ISA1 permanece compatível. O adaptador `p8_guest_graphics.py` associa invocações admitidas aos índices nativos, valida o binário guest, une as regiões escalares e os buffers efetivamente retidos, executa a ISA original e extrai POS0 e parâmetros. Dois padrões para registradores não observados são um teste de sensibilidade, não prova exaustiva de ABI. Não substitui a referência de admissão NGG, PS/material, cobertura ou o cone até o flip.

Teste host gráfico em execução na revisão ef607f84a924184d30e08b932bcf806e77410c40: https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/46012579033. O adaptador Python foi verificado sintaticamente; a execução real com P8ISA2 ainda não foi certificada. Os testes de regressão causal/transferência Vulkan da revisão de staging passaram (run 46012578899). Sem APK novo, sem corte P8.

Reprodução, após obter o executor P8ISA2:

    python3 docs/android/tools/p8_guest_graphics.py DIAGNOSTICS.zip /path/to/kyty_p8_guest_ref --draw 599 --max-waves 1 --output graphics.json

O resultado da comparação de posição não é promovido à decisão bilateral sem os contratos de todos os predecessores necessários e a propagação comprovada até o mesmo flip. Uma exportação PARAM esperada sem observação material equivalente permanece referência local, não divergência confirmada.


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
