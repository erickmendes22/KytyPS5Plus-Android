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
