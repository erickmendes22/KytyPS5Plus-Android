# V4: registros dinâmicos de luz — APK #758


## #757 — resultado real / contrato ativo #758

- ZIP 20261127-161050, SHA256 c6cde2375d5082e261711c022880d328dac0f499c73b6ae955151f60232a5e0f. Frame 383, submit 11412, P8_USE_84, q8 WG287 lane42, pixel (882,669), stencil 0x56, tile 16710.
- MATCH local: 64/64 coordenadas, stencil, depth; 192/192 leituras de material; 320/320 operações de máscara; 9/9 acumulações; 576/576 operações HDR RGB compatíveis. Não elimina outras waves/filas ou amostragem independente. P8 global permanece historicamente BLOCKED [0,16], zero metades globais eliminadas.
- Replay condicional da ISA original, com valores de amostras observadas e reduções já comparadas: RGB a até 2 ULP float32, mesmo FP16. A luz 0 possui RGB 0.00828048; luz 1, raio ~1.35 contra distância ~1.467; luz 2, flag 0x21 contra bit de categoria 0x20. Essas explicações intermediárias são derivadas da referência, não captura nativa dos loads escalares. A soma baixa não demonstra defeito de iluminação intencional.
- **Único contrato ativo #758:** backing guest estável e autorizado -> slot 2 (224 registros de 384 bytes) -> loads escalares dinâmicos da luz e decisão AND/EXEC. Capturar a mesma fonte antes/depois da cópia e após dispatch, propriedade/geração/offset, e registros realmente consumidos (inclusive rejeitados). MATCH encaminha ao produtor dos valores; DIVERGENCE no campo consumido autoriza localizar/corrigir a publicação ou load; INCOMPLETE não certifica autoridade. Sem alteração artificial de luz.
- Ver [DEMONS_APK_758_V4_LIGHT_RECORDS.md](DEMONS_APK_758_V4_LIGHT_RECORDS.md). #758 preparada, resultado de aparelho ainda não existe.


## Decisão executável
Comparar os bytes completos do slot 2 (86016 bytes), a identidade e a autoridade
do backing, o offset do objeto nativo e os campos escalares consumidos nos PCs
0xdf4/0xdf8/0xe68. A extensão mantém o trace #757, acrescenta um registro de
48 palavras por luz selecionada. Stencil 0x56 implica categoria 0x20. Flag
0x21 rejeita a luz pelo AND de categoria. A comparação do resultado e do EXEC
não depende de assumir que essa rejeição seja um erro do jogo.

* MATCH: campos nativos coincidem com os bytes guest autorizados e estáveis;
  loads/EXEC coincidem com a referência de mesma entrada. Próximo contrato:
  produtor específico dos registros ou da classificação, não o acumulador.
* DIVERGENCE: identificar a primeira palavra divergente e se é fonte->buffer
  ou buffer->load/EXEC. A diferença em campo não consumido não prova efeito.
* INCOMPLETE: identidade, extensão, estabilidade ou autoridade não comprovada.
  Ainda é possível comparar o contrato local load/EXEC com os bytes nativos,
  sem atribuir autoridade ao backing.

O comparador host `docs/android/tools/v4_q8_light_records.py` verifica os dados
e o teste correspondente cobre igualdade, alteração, rejeição e ausência.
O replay da #757 não modela de forma independente as texturas/shadows nem
o produtor guest. Não declarar correção visual antes de evidência real.
