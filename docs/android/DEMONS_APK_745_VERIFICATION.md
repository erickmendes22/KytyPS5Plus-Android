# APK #745 — captura decidível de posições e verificação

Data: 26/11/2026. A compilação terminou com sucesso e o APK original foi
baixado, extraído e verificado. O nome local foi alterado para identificar a
build; os bytes assinados não foram modificados.

## Identidade do artefato

| Campo | Valor verificado |
| --- | --- |
| Build | 745 |
| Revisão do núcleo | `4a94749e889ffd178418d347fcdd82ccc83ca73e` |
| Arquivo de entrega | `KytyPS5-Plus-745-4a94749.apk` |
| Tamanho | 16.415.598 bytes |
| Package | `com.kytyps5.plus` |
| versionCode / versionName | 845 / 0.1.0 |
| Android mínimo / target | API 31 / API 35 |
| Arquitetura | Seis bibliotecas ELF64 little-endian AArch64 |
| Assinatura | APK Signature Scheme v2 válida, identidade estável igual à #744 |

[Execução de compilação e testes](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45856724014).
[Artefato GitHub](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45856724014/artifacts/15136253638).
[Código compilado](https://github.com/erickmendes22/KytyPS5Plus-Android/commit/4a94749e889ffd178418d347fcdd82ccc83ca73e).

SHA-256 do APK:

```text
7d7a4bc144676e54d82f4e4388e34efd53603ccf66920521b411ef36c15058f2
```

SHA-256 do certificado:

```text
8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0
```

O ZIP baixado tem 14.514.870 bytes e SHA-256
`fb36aa9be373a505e63c4ea09ad9bb99f307c4cd0dbe623b3d69b21ab5126a4e`,
igual ao digest publicado pelo GitHub. A integridade CRC, a revisão completa
em `assets/native-core.json` e embutida em `libmain.so`, os marcadores da nova
captura, as dependências nativas e a política de ponteiros guest passaram.

Os hashes em `native-core.json` descrevem o cache/staging anterior à etapa
Gradle `:app:stripReleaseDebugSymbols`. Os hashes das bibliotecas finais do
APK são registrados separadamente na auditoria; não se exige igualdade entre
bytes anteriores e posteriores ao stripping. A assinatura foi verificada
diretamente no APK final.

## Alterações incluídas

1. O primeiro input é admitido após a identificação do clear executado. O
   orçamento inclui JSON e blobs, e o ciclo anterior não consome o orçamento
   da primeira operação do ciclo novo.
2. F captura as posições dos VS estruturalmente suportados do mesmo ciclo,
   os índices, os buffers nativos ligados e as leituras escalares efetivamente
   retornadas durante a materialização. Os buffers são lidos em lote por draw.
3. ISA guest é preservada inclusive em cache hits. São gravados tanto o
   SPIR-V original quanto o módulo instrumentado efetivamente ligado.
4. O exportador seleciona dependências obrigatórias antes dos opcionais,
   registra SHA-256, tamanhos e motivos de ausência, sem os antigos limites
   de quantidade de módulos para esse conjunto obrigatório.
5. O oráculo offline `prelight_vertex_oracle.py` calcula EXP POS0 pela ISA
   gfx1010 com os mesmos operandos. MATCH, DIVERGENCE e UNSUPPORTED são
   resultados por draw; nenhum desses nomes sozinho certifica P8 integral.

## Verificações antes e durante a compilação

- 23 testes do oráculo, incluindo mutações reais de instrução, buffer,
  constante, saída nativa e associação de índices.
- 23 testes do auditor local, incluindo as capturas reais #741/#744; a CI
  executou as fixtures e omitiu somente o teste que requer esses ZIPs externos.
- 14 testes do replay raster.
- 72 asserções do exportador real, incluindo 97 MiB obrigatórios, originais e
  módulos ligados distintos, slices binários, dados ausentes, antigos e
  alterações de tamanho/digest.
- 48 módulos instrumentados validados localmente, incluindo os 12 VS nativos
  disponíveis da #744; fixtures SPIR-V também passaram na CI.
- Regressão de SRT com callback mutante: captura do valor consumido, sem
  releitura, preservação do userdata, leituras recusadas e orçamento.
- Compilação Android nativa e Java/Kotlin, testes do aplicativo, assinatura,
  auditoria de dependências e auditoria da ABI de ponteiros passaram.

O target completo opcional de regressões nativas não foi acionado nesta
execução. Isso é distinto dos testes focados e da compilação do núcleo,
ambos concluídos com sucesso.

## Estado causal e próxima execução

O novo cálculo independente reproduziu na #744 **38 vértices e 152
componentes XYZW bit a bit**. A referência guest também alimenta o controle
raster do céu, que preserva zero divergências na máscara D32. Isso amplia
o certificado local; não demonstra correção da iluminação inteira.

G0/P8 continua sem decisão global comprovada. A #745 corrige o mecanismo que
impedia obter e comparar os operandos dos draws anteriores da mesma
testemunha. Os certificados e ramos já descartados são mantidos. Não houve
nova execução de gameplay no aparelho durante esta entrega.

Próximo passo no aparelho: instalar a #745 sobre a versão existente,
selecionar **F — Rastreamento causal disponível** na matriz causal, repetir
o mesmo ponto com o save/câmera/resolução/driver da #744 e exportar o ZIP de
diagnóstico após reproduzir o problema. A análise usa esse ZIP e os scripts
integrados; não exige captura de PC nem dump externo de assets.
