# #756 — captura real e revisão do método P8 (27/11/2026)

## Decisão operacional

Não gerar #757. P8 continua BLOCKED [0,16], zero metades cortadas. A aquisição
real permite construir a propagação inversa do flip, mas o adaptador bilateral
ainda não decide os dois lados. O usuário pediu interromper builds que não
levem à decisão, não abandonar a investigação. O roteiro passa a ser a
testemunha do flip, e não uma sequência de sondas ou certificados locais.

## Execuções reais

ZIP 20261127-130905, SHA256 5939751db8dba1970af22ed1451f67df442f6f5ffde66e2fda4e24d6dee3b7cf.
Revisão f6d40307f6087e53de9a1d67efca5d710356500b, alvo frame427, modo F.
27/27 arquivos do manifesto íntegros, 16 janelas sem descartes.
13.860 pacotes e 13.860 outcomes; 801/801 usos retidos
associados a um pacote com outcome executed (isso não é prova de completude
do censo semântico). 286/286 leituras da fronteira têm bytes exatos,
8/8 versões capturadas; 322 operações candidatas, 743 ocorrências de
predecessor não resolvido, 1136 ocorrências de binding de buffer sem contrato
de versão/acesso. O inventário não constitui cone fechado.

- CP: 8/8 registros comparáveis MATCH, 4 batches, nenhuma omissão ou erro de
  journal. As duas comparações pre-read de autoridade GPU cobrem 20 bytes
  cada; a comparação pós-read não prova produtor correto.
- HZB: 18/18 reduções min2x2 coincidentes, 349524 texels de saída, frames
  426/427. A profundidade de entrada 1024x512 é zero; zero correto na redução
  não prova que a origem guest da profundidade deveria ser zero.
- Tesselação seq8781 frame426 submit12618, registro331: 44980 primitivas,
  referência e observação [0,0,0,0]. Seq8782 é outro alvo do mesmo dispatch
  e não vincula o registro331; não classificá-lo como divergência.
- Draw783 frame427 submit12631, PS 552d823dcfcf2dee: nova execução do slice
  guest MRT0 (três MODEs, dois padrões de registradores, 27 controles
  full-vs-slice) reproduz 0xff000000. 1437175/1440000 palavras da fronteira
  P8_FULL_20.bin coincidem com esse valor. Versão48781/ordem198797,
  consumidor submit12635. RGB não zero 3471 antes, 2825 depois do draw.
  A cobertura e os produtores das constantes permanecem abertos.
- Compositor P8_USE_418, packet10387, frame427 submit12635: o replay
  original para por DCC na descrição da imagem de overlay; após um teste
  de normalização física apenas no host, o executor rejeita image_sample
  em PC 0x70. Não promover essa falha a um defeito do jogo.
  Nova referência analítica de pixels lógicos, derivada da ISA original
  3265b01ed00a5c01, com fingerprint de módulos, descritores, samplers, estado
  raster, escala e versões: P8_FULL_31.bin overlay v65882, P8_FULL_32.bin
  cena v1326 ordem198985, P8_FULL_33.bin flip v389 ordem199113.
  2066048/2073600 palavras exatas; 7552 com delta RGB máximo 1 LSB de 10 bits,
  zero com delta maior. Três controles de cena uniforme (0, 0.25, 1)
  reproduzem 0xc0000000, 0xe2589625, 0xffffffff. O cálculo analítico usa
  filtragem bilinear em pixels já decodificados, não certifica DCC nem
  precisão exata do sampler hardware; nenhum certificado global.
- Testemunha fixada no flip: pixel (960,540), overlay RGBA=0,
  referência e observação 0xc0600000. Uma alteração sintética exclusiva
  no suporte 7x7 de cena em torno de (800,450) muda a referência final para
  0xe2589625. É controle de sensibilidade, não correção, não verdade guest.
  A saída final é compatível com a cena escura consumida.

## Substituição do fluxo de trabalho

1. Manter P8 como único nó global. Fixar pixel e identidade/versão do flip
   e reaproveitar o replay do sufixo comprovado. Mapear o suporte real da
   amostra para cada produtor anterior. Não exigir que toda operação do
   frame, fora desse suporte, seja executada independentemente.
2. A aresta imediata é a produção de cena 0x2b2a40000 v1326. P8_USE_242
   (packet9633, PS a5fc1658592e3cba) e P8_USE_243 (packet9653,
   PS b4e1b94988dff9ae) são os candidatos escritores observados. O segundo
   consome como attachment v1324/ordem198984. Faltam os estados e operandos
   escalares guest e os bytes versionados das imagens de entrada: o primeiro
   tem oito bindings de imagem e o segundo vinte bindings PS mais um VS.
   A referência do compositor aceita a saída já escura, logo não adquirir
   novamente o compositor. Examinar os dumps existentes e fechar as
   dependências do pixel nesses dois escritores, incluindo pós-processo
   HDR v9821, P8_USE_161 e 181 (ambos vinculados a seus pacotes).
3. Para cada predecessor, primeiro reproduzir o baseline no mesmo pixel
   e na mesma versão; só depois propagar uma substituição independente
   através do sufixo já validado. Se o valor esperado divergir do observado
   e afetar o flip, localizar a primeira divergência. Se uma diferença
   surgir após C8, ainda certificar o prefixo causal relevante antes de
   declarar PASS global. Se aparecer em C1..C8 e o efeito até o flip for
   demonstrado, FAIL global. Não aceitar uma alteração sintética como
   contraexemplo real.
4. O gate atual de inventário ainda retorna BLOCKED por construção.
   Integrar os contratos ao decisor e provar ambos os resultados em replay
   com o caso identificado; testar omissão de produtor, versão errada,
   referência igual, divergência sem propagação e diferença com efeito.
   Não compilar uma sonda enquanto os dois caminhos não forem executáveis.
   Se os operandos atuais realmente não bastarem, delimitar uma aquisição
   única dos produtores e consumidores do mesmo suporte, com exportação
   e verificação previamente exercitadas. Não exigir PC idêntico,
   replay universal, nem o resultado do aparelho antecipadamente.

## Reprodução

    python3 docs/android/tools/p8_compositor_cpu.py DIAGNOSTICS.zip
    python3 docs/android/tools/p8_material_mrt0.py DIAGNOSTICS.zip /path/to/kyty_p8_guest_ref --draw 783 --check-fixtures
    python3 docs/android/tools/p8_tessellation_count.py DIAGNOSTICS.zip --seq 8781
    python3 docs/android/tools/p8_cp_ordered.py DIAGNOSTICS.zip

O executor usado é o artefato p8-independent-guest-isa do run 46012579033,
e os scripts anteriores foram recuperados do run 46025386665. A referência
analítica e a seleção dinâmica do draw foram executadas localmente no ZIP
real. Não há mudança do APK, correção visual ou decisão de P8 alegada.
