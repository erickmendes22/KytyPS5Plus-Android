# V4 — contraexemplo local e recuperação mensurável (27/11/2026)

## Decisão

O usuário autorizou expressamente substituir o método se não for possível
certificar o prefixo bilateral de P8. A captura #756 não permite isso:
743 ocorrências de predecessores não resolvidos, 1136 ocorrências de
bindings de buffer sem contrato de versão/acesso e nenhuma referência
independente de todos os roots relevantes. O commit histórico 838d12c8
é referência de rosto visível, não de iluminação saudável. Aumentar o
replay global não é uma estratégia de redução garantida.

**P8 permanece historicamente BLOCKED [0,16], zero cortes globais.**
V4 substitui a obrigatoriedade de decidir P8 antes de corrigir o jogo.
A classificação C1..C16 permanece útil para localizar a operação provada;
não se exige certificar todos os ancestrais para corrigir uma violação
local reproduzível. Não chamar uma evidência local de PASS/FAIL de P8.
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

## Roteiro operacional: primeiro contraexemplo, não a prova universal

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
   os desfechos apontam ação concreta. Não usar o valor "BLOCKED"
   de P8 como veto geral nem removê-lo artificialmente.

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
