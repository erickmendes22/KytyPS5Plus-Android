# KytyPS5 Plus Android


## V4 — método único e obrigatório (confirmação do usuário, 27/11/2026)

**P8/P(8) foi descartado como modelo de investigação.** V1–V3, a
bisseção C1..C16, os prefixos PASS/FAIL e o intervalo [0,16] não são
objetivos, métricas, pendências, condições de entrega nem roteiro ativo.
Não reportar o estado do P8 em análises, árvores ou balanços futuros.
Não produzir novas capturas para decidir a antiga divisão. Os nomes
técnicos legados (por exemplo P8_USE e ferramentas de captura) podem
ser reutilizados como infraestrutura de evidência, sem reativar o método.

Usar exclusivamente o [contrato local V4](docs/android/DEMONS_V4_LOCAL_CONTRACT.md):
partir do sintoma concreto e seguir as dependências causais da operação
testemunha. Comparar a mesma operação, entradas, versão e saída com
referência independente; um contrato ativo por vez.
- MATCH: fechar a operação no escopo demonstrado e seguir o produtor
  do operando que explica a saída observada.
- DIVERGENCE: isolar a primeira divergência demonstrada, corrigir a
  implementação, testar regressão e medir efeito sobre o sintoma.
- INCOMPLETE: reparar a aquisição ou referência local antes de outra
  build; não repetir inventário nem mudar de ramo sem prova.
Preservar os resultados úteis anteriores no escopo original. Cada build
deve resolver uma pergunta indispensável à correção, com comparador
validado e dois desfechos acionáveis. A continuidade e entrega de APK
permanecem obrigatórias. Mudanças exclusivamente metodológicas não
disparam compilação.

> [!IMPORTANT]
> **AVISO CRÍTICO — cumprimento obrigatório do AGENTS.md (27/11/2026).**
> Este arquivo contém requisitos operacionais, não sugestões. É obrigatório
> lê-lo e aplicá-lo integralmente no início de cada tarefa, inclusive quando o
> usuário disser apenas "continue" ou retomar uma investigação em outro chat.
> Não cabe ao usuário repetir estas regras ou pedir novamente que o trabalho
> prossiga.

## Verificação obrigatória antes de encerrar qualquer tarefa

Para uma tarefa em andamento cujo entregável seja um APK, verificar **antes
de emitir uma resposta final**:

- [ ] Todas as instruções aplicáveis deste AGENTS.md foram cumpridas, inclusive
  o contrato V4 e a matriz causal atual.
- [ ] O objetivo solicitado foi implementado e validado no escopo possível;
  falhas recuperáveis e lacunas de implementação foram tratadas.
- [ ] O APK da revisão correta foi compilado, baixado, assinado e verificado;
  o link real de download está pronto para entrega.
- [ ] A árvore e o balanço de eliminação foram atualizados quando aplicáveis,
  distinguindo resultados reais, certificados locais e validação em gameplay.

Se qualquer requisito aplicável estiver pendente, continuar executando as
próximas ações disponíveis na mesma tarefa. Não encerrar com um relatório de
progresso, commit, teste isolado, build iniciada, diagnóstico inconclusivo, plano de trabalho
ou promessa de continuar. Não pedir autorização para continuar o trabalho já
solicitado. Usar mensagens intermediárias apenas para informar progresso e
prosseguir com a execução.

As únicas exceções são a ordem explícita do usuário para interromper ou um
impedimento externo efetivamente irrecuperável/limitação real da sessão,
demonstrado com a operação tentada, o erro observado e as alternativas
esgotadas. Trabalho incompleto, comparador ainda não implementado e falhas
recuperáveis não são impedimentos externos. Não inventar resultados nem
substituir evidência por afirmações. Não exigir resultados de gameplay antes
da entrega de uma build diagnóstica cujo instrumento já esteja validado.

Esta checagem reforça, sem substituir ou relaxar, todas as regras abaixo.
Tarefas explicitamente limitadas a documentação, revisão ou outra entrega sem
APK conservam seu escopo próprio; não disparar builds desnecessárias.

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

## Balanço obrigatório de eliminação por build

Manter `docs/android/DEMONS_BUILD_ELIMINATION_LEDGER.md` a cada APK, conforme
instrução explícita do usuário de 26/11/2026. Registrar a revisão, o nó/aresta
ativo, a captura analisada, os ramos ou operações efetivamente eliminados,
a prova e seu alcance, o que permanece ativo e a próxima decisão.

**Se nenhum ramo foi cortado, escrever isso diretamente.** Explicar a causa
concreta: operando ausente, identidade incompatível, retenção incorreta,
referência sem autoridade, comparador insuficiente, resultado temporalmente
instável ou outra falha demonstrada. Registrar a correção dessa deficiência e
a verificação que a próxima aquisição deve satisfazer para produzir o corte.
Não usar uma nova hipótese ou mais medidores como substituto dessa correção.

Separar corte de um ramo inteiro de certificado de uma operação local.
Revalidação de certificado antigo não é um corte novo. Compilar, assinar,
passar testes e entregar uma sonda também não elimina uma causa do jogo.
APK ainda não executado no aparelho fica explicitamente aguardando captura;
nunca registrar seu corte previsto como resultado obtido.

Antes de compilar a aquisição seguinte, conferir que os dois resultados da
comparação têm referência/operandos, vínculo ao mesmo caso, retenção, orçamento,
exportação e comparador capazes de distingui-los. Exercitar igualdade, divergência
e os modos concretos de falha da aquisição anterior. Se a execução não decidir,
o próximo trabalho deve reparar a lacuna identificada, sem alternar o galho.
Isso não exige oráculo integral do jogo, PC idêntico ou rastreador universal CPU
como pré-condição para uma aresta local decidível.

## Investigação causal sequencial (obrigatória, precede a escolha de sondas)

Não usar uma sucessão de hipóteses independentes nem uma build por suspeita.
Manter um caso-testemunha com identidade (frame/submit, endereço e conteúdo
de índices, draw, shaders, instância, versões dos recursos, câmera/constantes)
e uma saída observada. Selecionar caso que contribua para o sintoma, mais um
controle positivo da mesma etapa; ausência de cobertura não prova que o
objeto deveria estar visível. Não mudar o caso por mera correlação.

Partir da perda observável relevante à testemunha (radiância, material,
cobertura ou outra saída demonstrada). Rastrear o DAG das dependências
reais até a primeira divergência; não impor uma sequência fixa de passes
ou assumir que a causa está na geometria ou no albedo. Para cada aresta, registrar
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

## Árvore causal ativa V4

A fonte normativa é DEMONS_V4_LOCAL_CONTRACT.md; registrar o DAG atual
em DEMONS_PRELIGHT_BINARY_TREE.md (nome legado) e atualizar a matriz e
o balanço de eliminação a cada captura/build. A árvore contém contratos
com evidência e escopo, o único contrato ativo e sua decisão seguinte.
Não usar bisseção, prefixos cumulativos ou cortes globais como métricas.
Conservar as evidências históricas sem tratá-las como instruções atuais.
