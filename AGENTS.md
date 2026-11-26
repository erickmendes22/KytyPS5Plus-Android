# KytyPS5 Plus Android

Para tarefas de correção ou diagnóstico Android que pedem uma nova build, o
entregável é um APK assinado e instalável. Continue até compilar, acompanhar a
execução, corrigir falhas recuperáveis, baixar o APK, verificar a revisão e a
assinatura e entregar o link. Código integrado e testes locais são etapas
intermediárias. Se houver um bloqueio externo irrecuperável, informe a etapa
e a falha observada.

Cada build deve trazer uma correção fundamentada ou uma captura que responda
a uma pergunta concreta indispensável à correção. Identifique o alcance do
APK e informe quais resultados ainda precisam de validação em gameplay.

## Regra obrigatória de continuidade e encerramento

**Não parar por iniciativa própria.** Depois que o usuário solicitar uma correção
com entrega de APK, continuar a investigação, implementação, validação e
compilação até que **pelo menos uma** destas condições seja satisfeita:

1. O usuário dê uma ordem **explícita** para parar ou interromper a tarefa; **ou**
2. O APK solicitado esteja **compilado, assinado, verificado e entregue** ao usuário
   com um link real para baixar.

**Investigação parcial, diagnóstico inconclusivo, alterações de código, commit,
compilação iniciada, falhas recuperáveis, ausência de ganho confirmado ou demora
na execução NÃO autorizam encerrar a tarefa.** Continue procurando a causa real,
corrigindo e acompanhando as etapas necessárias, sem produzir builds apenas por
produzir. Uma resposta intermediária deve informar o progresso e prosseguir com
o trabalho, não substituir a entrega.

Se houver um bloqueio externo efetivamente irrecuperável ou uma limitação real
de acesso/ferramentas da sessão, descreva-o de maneira verificável e sem alegar
que o APK foi entregue. Não invente conclusão, resultado de build ou link; retome
assim que a execução estiver disponível. Esta ressalva de transparência não é
uma autorização para abandonar voluntariamente a investigação.

## Matriz causal obrigatória — Demon's Souls PPSA01341
Meta: isolar a primeira divergência em poucas builds. M1 visibilidade, M2
emissão de geometria, M3 memória/streaming/coerência, M4 primeira perda de
albedo (prioridade máxima); M5 radiância, M6 ordenação, M7 semântica do
recompilador (alta). Atualizar docs/android/DEMONS_CAUSAL_MATRIX.md. Não
reabrir hipóteses descartadas sem nova evidência. Comparar o mesmo recurso,
objeto, operação, versão e dados de entrada, não cenas diferentes ou meras
contagens globais. Registrar produtor, bytes, tick, consumidor, saída
esperada e observada. Não tratar zero válido ou dados ausentes como erro.
PC U59 dd9f990 e Chenxiao07 66cdb39 são referências de código, não capturas
de gameplay comprovadamente idênticas.

A controle, B1/B2 culling original alternativo separado, C1/C2 coerência,
D1/D2 ordenação, E reprodução independente de operações selecionadas,
F rastreamento causal. Um APK, modos independentes; manter mesmo save,
câmera, resolução e driver. B/C/D são experimentos de isolamento, não fixes
definitivos. E não implica replay integral quando faltam dados. Nenhuma
alteração artificial de luz, depth ou contagens. Cada divergência precisa
de prova causal e teste de regressão antes de virar correção de produção.
Build de isolamento, build de correção fundamentada, consolidação se
necessária. Entregar cada APK conforme regra de continuidade acima.


## Investigação causal sequencial (obrigatória, precede a escolha de sondas)

Não usar uma sucessão de hipóteses independentes nem uma build por suspeita.
Manter um caso-testemunha com identidade (frame/submit, endereço e conteúdo
de índices, draw, shaders, instância, versões dos recursos, câmera/constantes)
e uma saída observada. Selecionar caso que contribua para o sintoma, mais um
controle positivo da mesma etapa; ausência de cobertura não prova que o
objeto deveria estar visível. Não mudar o caso por mera correlação.

Partir da primeira perda observável no albedo, rastrear o mesmo draw para
trás: material -> prepass/depth e rasterização -> posição/clip de cada
vértice -> cálculo do VS -> loads efetivos, endereços e dados -> produtor
real desses bytes -> comando/asset guest. Para cada aresta, registrar
entrada esperada/observada, operação guest e Android/PC, saída esperada/
observada, versão e identidade do produtor/consumidor. Se a saída é
compatível com a entrada, seguir imediatamente para a origem dessa entrada;
se diverge, reduzir a primeira instrução/transferência divergente, corrigir
e testar regressão. Se faltar evidência, registrar exatamente qual aresta
está bloqueada e adquirir em uma única captura os operandos necessários
dela e das próximas arestas possíveis. Não reabrir etapas já certificadas.
Comparar bytes/SSA/valores reais; não usar apenas contagens agregadas, hashes
de shader, frames com câmeras diferentes, ou PC sem a mesma entrada como
oráculo. Não tratar zero de cobertura como defeito por si só.

Antes de disparar nova build, esgotar o replay offline dos ZIPs e listar o
resultado de cada aresta (validada, divergente, bloqueada). Uma nova build
diagnóstica deve fechar a próxima aresta do caso fixo, com captura do
produtor e consumidor, orçamento e validação do exportador, e não apenas
adicionar medidores. Não disparar builds paralelas para testar suspeitas.
Quando houver divergência comprovada, parar a expansão diagnóstica e
implementar a correção fundamentada com teste de regressão. As regras
de entrega de APK permanecem em vigor.

## Árvore binária pré-iluminação (obrigatória)
Usar docs/android/DEMONS_PRELIGHT_BINARY_TREE.md como árvore operacional
canônica antes de escolher nova investigação de Demon's Souls. Resolver
N00 pela primeira divergência de albedo em um ciclo identificável; marcar
cada nó S/N com prova de mesma entrada/saída ou BLOQUEADO. Não avançar
por suposição, não retestar os nós podados P01–P06 sem nova evidência
incompatível, não criar builds por suspeita. Se há mais de uma testemunha
com divergência, manter ambas as trajetórias e ordenar os produtores.
Atualizar a árvore com resultados de cada execução e só implementar fix
na primeira operação divergente demonstrada. Construir a árvore, quando
isso for a etapa explicitamente solicitada, não exige nova compilação.
