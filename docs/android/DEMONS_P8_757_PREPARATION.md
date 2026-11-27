# Preparação #757 — 27/11/2026

Referência: diagnóstico real #756, frame 427, revisão f6d4030. O único
nó global continua P8 [0,16], BLOCKED, zero metades eliminadas.

## Implementação host

- `tools/p8_image_trap.py`: executa ISA original por meio do runner ROCm,
  capturando dinamicamente coordenadas, descritores e lanes por visita
  de MIMG; amostragem lógica 2D independente; rejeita instruções,
  formatos, visitas e dados não suportados. Trampolins apenas na cópia
  executada pela referência.
- `tools/p8_compositor_isa.py`: executa o shader real do compositor,
  valida estado, módulos, identidades de imagens e vínculo ao flip.
  Coeficientes LDS corrigidos e validados em quatro lanes.
- `tools/p8_bilateral.py`: contrato executável compositor_pixel_isa
  para propagação de versões substituídas até a observação final.

## Testes locais executados

Com o runner do artifact 15206248773, ZIP real #756:

    python tools/test_p8_image_trap.py /path/to/kyty_p8_guest_ref
    python tools/p8_compositor_isa.py /path/to/diagnostic.zip /path/to/kyty_p8_guest_ref
    python tools/test_p8_actual_suffix.py /path/to/diagnostic.zip /path/to/kyty_p8_guest_ref

64/64 amostras distribuídas e 4/4 pixels do quad central exatos.
Pixel 960,540: 0xc0600000 esperado e observado. Suporte real: 16
texels da cena 0x2b2a40000 v1326 e um do overlay 0x281a80000 v65882.
Controle sintético fora do suporte não altera o pixel; alteração dentro
do suporte produz 0xe2589625. Os dois caminhos do motor bilateral
executam o compositor real; FAIL [0,8], caminho 1,2,3, e PASS [8,16],
caminho 2,3; luminância corrigida 0.5366568914956011 em ambos.
Quatro controles negativos passaram. Operações e valores anteriores
ao compositor nesses testes são artificiais e não evidência de erro
do jogo. Testes não alteram o ZIP nem o jogo.

## Bloqueio de aquisição / referência

P8_USE_242 (a5fc1658592e3cba) e P8_USE_243 (b4e1b94988dff9ae)
produzem a cena observada. A captura #756 não contém todos os
operandos guest desses dois escritores, estado de admissão/cobertura
e dados de imagem por versão. Há arrays BC3/BC7 de centenas de layers,
mips e texturas 3D no segundo; ler tudo excederia o orçamento e
repetiria o problema #748. O modelo lógico atual suporta apenas
2D mip0, não certifica decodificação física/DCC, precisão do sampler
ou autoridade de produtores. O adaptador real ainda não executa a
cadeia de C1–C8 até o compositor; fixtures não substituem essa etapa.

Condições de liberação #757: referência independente para ambos os
escritores com cobertura e dados versionados do suporte efetivo;
ligação real ao corte C8 e a uma testemunha final; contraexemplo de
prefixo propagável para FAIL ou prefixo verificado e contraexemplo
posterior propagável para PASS; testes de perda de dados e versões
em aquisição. Nenhum novo APK iniciado por estas alterações host.
