# APK #746 — argumentos indiretos e verificação da entrega

Data: 26/11/2026. A build terminou com sucesso. O APK original foi baixado,
extraído e verificado; apenas o nome do arquivo de entrega foi alterado.

## Identidade verificada

| Campo | Resultado |
| --- | --- |
| Build | 746 |
| Revisão compilada | `4e321c2dbedcab17085c1af6ecf0d0f4f8345afe` |
| APK | `KytyPS5-Plus-746-4e321c2.apk` |
| Tamanho | 16.473.458 bytes |
| Package | `com.kytyps5.plus` |
| versionCode / versionName | 846 / 0.1.0 |
| Android mínimo / target | API 31 / API 35 |
| Arquitetura | Seis bibliotecas ELF64 little-endian AArch64 |
| Assinatura | APK Signature Scheme v2 válida; certificado estável igual à #744/#745 |

[Build e testes — run 45866254600](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45866254600).
[Artefato GitHub](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45866254600/artifacts/15139639551).
[Código compilado](https://github.com/erickmendes22/KytyPS5Plus-Android/commit/4e321c2dbedcab17085c1af6ecf0d0f4f8345afe).

SHA-256 do APK:

```text
34f973838bbd3df786ca02c989f54f8c61b6cba38d67217eda0b766ff42e7c6c
```

SHA-256 do certificado:

```text
8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0
```

O ZIP do artefato tem 14.568.151 bytes. Seu SHA-256 é
`deb1bd68dffc395304004408de672627a7cfc7f624e5c7b4d5a83d7c7db5fca7`,
igual ao digest publicado pelo GitHub. CRC, revisão completa em
`assets/native-core.json` e em `libmain.so`, marcadores da captura nova,
assinatura final, dependências nativas e ABI de ponteiros guest passaram.

Os hashes em `native-core.json` descrevem o staging/cache anterior ao stripping
do Gradle. Os hashes das bibliotecas finais foram calculados separadamente;
diferença entre essas etapas não foi confundida com núcleo incorreto.

## O que esta build entrega

A [análise da #745](DEMONS_APK_745_RESULTS.md) reproduziu 45 posições finais
de draws, somando 73.580 vértices e 294.320 componentes XYZW bit a bit.
O próximo dado ausente está nos três registros de argumentos do batch CPU
observado em `0x21de00078`, que passou de 98.304 índices para zero.

A #746 preserva os usos originais dos DWORDs e o tuple emitido, congela a
referência independente por registro e coleta as cópias GPU depois dos três
consumos. O estado no enqueue fica separado das publicações durante a coleta.
Último não zero e primeiro zero são retidos até o trigger de perda de albedo,
com produtores, transferências e dependências de exportação fora do ring dos
50 draws. O [contrato de aquisição](DEMONS_CP_INDIRECT_CAPTURE.md) descreve
autoridade por byte, orçamento, tratamento de instabilidade e decisão offline.

O crash posterior de `ComputeTextureDesc` na #745 recebe os operandos do
descritor no log antes do guard original. O guard permanece com o mesmo
contrato; esse registro não é apresentado como correção de iluminação.

## Validação concluída

- Oráculo indireto: **40 testes**, incluindo divergência real de leitura,
  emissão incorreta, referência GPU antiga, autoridade mista, pending, ordem,
  retenção, transporte e resultados parciais. Passaram localmente e no CI.
- Oráculo de posição atualizado: **34 testes**, locais e no CI; replay real
  da #745 documentado separadamente.
- Replay raster: **14 testes no CI**. Auditor V3: 23 testes previstos,
  **22 executados e um skip** que requer ZIPs externos ao runner.
- Política C++: seleção, retenção móvel, coleta depois dos três consumos,
  publicação que altera memória, limites e digest; compilação rigorosa e asserts
  passaram localmente e no job de regressões.
- Exportador Java real: **15 fixtures / 79 assertions**, mais **12 assertions
  com os ZIPs #741/#744/#745**, passaram na validação local. Os testes do app
  também passaram no job Android.
- Compilação nativa, `testDebugUnitTest`, `assembleRelease`, assinatura e as
  auditorias de dependências/ponteiros passaram. O job completo opcional de
  regressões nativas permaneceu desativado, conforme o fluxo de build existente.

## Balanço causal e próximo teste

**Estado: COMPILADO, ASSINADO E VERIFICADO / AGUARDA CAPTURA DO APARELHO.**
Nenhum corte causal é atribuído ao simples sucesso desta build. A divisão
global P(8) permanece aberta. O [balanço por build](DEMONS_BUILD_ELIMINATION_LEDGER.md)
registra o alcance exato da #745 e a ação de cada resultado da #746.

No aparelho, instalar a #746 sobre a versão existente, selecionar
**F — Rastreamento causal disponível**, repetir o mesmo save/câmera/resolução/
driver da #745 e exportar o diagnóstico após o episódio de perda. Esse ZIP
permite confrontar fonte, valores consumidos e argumentos emitidos. Uma
igualdade elimina essa leitura/emissão no escopo comparado e direciona a
análise ao produtor retido; uma divergência válida fixa a operação a corrigir.

As alterações posteriores em `AGENTS.md` e nos registros de prestação de
contas são documentais. Não geram outro APK nem mudam a revisão embarcada
identificada acima.
