# APK #748 — aquisição P8 integrada e verificação de entrega

Data: 26/11/2026. Build concluída, APK baixado e verificado independentemente. Apenas o nome do arquivo extraído foi alterado.

| Campo | Resultado |
| --- | --- |
| Build | 748 |
| Revisão compilada | `0f1709763e30ab7812652d2eda81518a6e32ebff` |
| APK | `KytyPS5-Plus-748-0f17097.apk` |
| Tamanho | 18.488.738 bytes |
| Package | com.kytyps5.plus |
| VersionCode / VersionName | 848 / 0.1.0 |
| Android mínimo / target | API 31 / API 35 |
| Arquitetura | Sete bibliotecas ELF64 little-endian AArch64, incluindo a camada GFXReconstruct |
| APK SHA-256 | `a7d510a82a7f2a4fbcbbe7eb5255f9ca15759fd5758ec271ab1dfe71a090b76f` |
| Certificado SHA-256 | `8f5f176fc9fc9654de485ac73a8d1873ceb2b61122fd93f9da95717aa1309ee0` |

[Build Android 45905112907](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907) — regressões rápidas e build concluídas com sucesso; regressões completas opcionais desativadas. [Artefato 15157339392](https://github.com/erickmendes22/KytyPS5Plus-Android/actions/runs/45905112907/artifacts/15157339392). [Commit compilado](https://github.com/erickmendes22/KytyPS5Plus-Android/commit/0f1709763e30ab7812652d2eda81518a6e32ebff).

O ZIP baixado contém exatamente um APK, tem 16.557.277 bytes e SHA-256 `d1d56e03f0fc8c9cf02656027300a70ae54826add11dd002a4ff70cfa86c5812`, igual ao digest publicado no GitHub. CRC de ambos os ZIPs, assinatura RSA PKCS#1 v1.5 SHA-256 do bloco v2, digest de conteúdo v2 por chunks, igualdade do certificado de atualização, metadados binários do manifest, revisão completa em assets/native-core.json e libmain.so, ELF64 AArch64 e presença da camada GFXReconstruct verificados localmente. O CI também aprovou os testes unitários Android, assembleRelease e auditoria das dependências nativas.

## Balanço causal

O [registro de eliminação](DEMONS_BUILD_ELIMINATION_LEDGER.md) documenta o novo certificado local de 120/120 bytes do produtor de compactação da #746, executado pelo backend AMD independente. Sem corte global de P8. A #748 adquire os bytes HZB dos dois produtores de visibilidade que faltavam no ZIP anterior, além do censo de usos, referências e retenção dos candidatos sem alteração. Nenhuma correção visual ou divisão global é atribuída à compilação.

A validação host adicional executou a instrução image_sample_l original com os nove mips e coordenadas distintas em um HZB sintético. O armazenamento foi reempacotado para o layout linear RDNA1 do executor (mips menores primeiro, linhas alinhadas a 256 bytes), com Z_FILTER inativo na amostragem 2D removido somente na referência. As comparações de digest, mip ausente e versão alterada rejeitaram fixtures inválidos. No produtor 7d5ad40fca3137e2 do frame 215, seq 4866, os buffers capturados são reais; as imagens HZB de controle são sintéticas, constantes 0 e 1. A execução serial dos seis workgroups em ordem direta e reversa obteve, respectivamente, indexCount 344232 e 344064, iguais entre as duas ordens para cada fixture. Isso demonstra sensibilidade do produtor ao HZB e prepara a referência local; não prova o conteúdo HZB real, equivalência de todas as interleavings nem P8. Não converter o match acidental de 344232 no fixture em certificado do frame real.

**Estado: COMPILADO, ASSINADO, VERIFICADO E ENTREGUE / AGUARDA CAPTURA DO APARELHO.** Instalar sobre a versão existente, selecionar F, repetir mesmo save/câmera/resolução/driver e exportar o diagnóstico após o episódio de perda. Não exigir PC externo nem validação prévia de gameplay para a entrega do APK. Comparar as entradas efetivamente capturadas e preservar o alcance local dos certificados. As alterações documentais após a compilação não alteram a revisão do APK.
