# P8: corregir a fronteira observada (staging, não APK)

O diagnóstico #746 mostrou que a primeira queda do albedo não representa toda a
saída apresentada. Depois das filas 0/8/33 há outros escritores de HDR.
A camada SceneAddress é overlay; seu RGB zero não demonstra ausência de luz.

Esta revisão corrige a aquisição de P8 em vez de perseguir novos produtores
locais: grava as três camadas adicionais e profundidade antes da iluminação,
preserva identidades/versões, registra entradas e saídas reais do compositor e
a identidade da superfície resolvida para o flip. Uma janela rotativa mantém
16 frames candidatos de amostragem periódica mesmo após o fechamento do
journal antigo. O monitor passa a ler a entrada de cena pós-processada,
não o overlay. Amostragem é diagnóstico, não um valor esperado.

Limites: o registro ainda não cobre todos os pixels, dependências e escritores
que cruzam P8, e não há um backend independente que produza o G-buffer
esperado para o mesmo estado guest. Portanto esta alteração **não decide P8**,
não autoriza uma nova build de investigação e não permite selecionar C1–C8
ou C9–C16. Não interpretar igualdade de versões como correção. Antes de
promover esta revisão ao branch de APK, completar a comparação independente
da fronteira para a testemunha escolhida e a ligação causal à saída final.
Não exigir captura de PC do usuário, nem corrigir crashes paralelos como
substituto de resolver P8.
