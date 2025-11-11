# TomasuloSimulator

## Execução

Os arquivos `source.txt` e `source.exe` devem estar no mesmo diretório.

1- Abrir um terminal PowerShell  
2- Executar o comando: `./source`

Para avançar os ciclos do programa, pressione `Enter` a cada ciclo.

---

## Structs

### Gerais

#### struct Registrador

Define um registrador com um nome e um valor numérico.  
É usado tanto para o banco de registradores (`vector<Registrador> registradores`) quanto para representar posições de memória (`vector<Registrador> memoria`).  
O operador `==` é sobrecarregado para comparar dois registradores pelo nome, permitindo buscas e atualizações simplificadas.

---

#### struct StatusInstrucao

Mantém o estado de cada instrução ao longo da simulação, incluindo:

- `emitido`: ciclo em que foi emitida (issue)
    
- `inicioExecucao`: ciclo em que começou a execução
    
- `fimExecucao`: ciclo em que terminou a execução
    
- `escritaResultado`: ciclo em que realizou o _write-back_
    
- `ciclosRestantesExecucao`: contador de ciclos restantes até o término da execução
    

Essa estrutura é essencial para acompanhar o progresso e calcular as dependências entre instruções.

---

#### struct TiposInstrucao

Contém constantes de texto representando os tipos de instruções aceitas pelo simulador:  
`ADD`, `SUB`, `MUL`, `DIV`, `LOAD`, `STORE`, e `BNE`.  
Essas strings são utilizadas para identificar e direcionar o tratamento correto de cada instrução dentro do algoritmo de Tomasulo.

---

#### struct TipoEstacaoReserva e struct TipoBufferLS

Definem rótulos para as diferentes unidades funcionais usadas na simulação:

- **TipoEstacaoReserva**: define categorias de estações de reserva, como `ADD/SUB` e `MUL/DIV`.
    
- **TipoBufferLS**: define categorias de buffers de memória, `LOAD` e `STORE`.
    

Esses tipos ajudam a organizar e diferenciar as unidades funcionais de acordo com a operação que executam.

---

#### struct Instrucao

Representa uma instrução completa do programa de entrada.  
Contém:

- `tipoInstrucao`: tipo da operação (ADD, MUL, LOAD etc.)
    
- `regDestino`: registrador de destino
    
- `regFonte1` e `regFonte2`: registradores de origem ou base
    
- `offsetImediato`: deslocamento usado por instruções de memória e branch
    
- `status`: instância de `StatusInstrucao`
    

Cada objeto dessa struct descreve uma linha do arquivo `source.txt`.

---

#### struct EstadoRegistrador

Mantém o status de cada registrador físico.  
O campo `unidadeEscritora` indica qual unidade funcional (estação de reserva ou buffer) produzirá o valor do registrador, implementando o controle de dependências **RAW** (Read After Write).  
É equivalente ao campo Q.i no algoritmo de Tomasulo clássico.

---

#### struct EstacaoReserva

Modela uma estação de reserva associada a operações de ponto flutuante ou inteiras (ADD, SUB, MUL, DIV, BNE).  
Campos principais:

- `ocupado`: indica se a estação está em uso
    
- `tipoInstrucao`: operação sendo executada
    
- `valorJ`, `valorK`: valores dos operandos (Vj e Vk)
    
- `origemJ`, `origemK`: dependências (Qj e Qk), indicando de onde virão os operandos
    
- `instrucao`: ponteiro para a instrução associada
    

Essa estrutura permite execução fora de ordem e resolução dinâmica de dependências de dados.

---

#### struct BufferLS

Modela os buffers de LOAD e STORE usados para acesso à memória.  
Campos principais:

- `ocupado`: indica se está em uso
    
- `endereco`: endereço calculado (texto para exibição)
    
- `origemRs`, `valorRs`: dependência e valor do registrador base (Rs)
    
- `fu`: valor ou dependência do dado a ser armazenado (em STORE)
    
- `hasForward` e `forwardValue`: usados para implementar _store-to-load forwarding_, permitindo que LOADs obtenham valores diretamente de STOREs anteriores.
    
- `instrucao`: ponteiro para a instrução associada
    

Esses buffers garantem a manutenção da ordem de acesso à memória e tratam _hazards_ entre LOAD e STORE.

---

### Struct Tomasulo

#### Método emitirInstrucao

Responsável por emitir instruções da fila de entrada.  
Seleciona o tipo de unidade funcional apropriada (ER ou Buffer) e inicializa suas estruturas com os operandos e dependências.  
Verifica **hazards estruturais** (falta de unidade livre) e **hazards de dados** (dependências via Q.i).  
Instruções `BNE` usam uma estação de ADD/SUB para executar a comparação e travam a emissão de instruções subsequentes até o branch ser resolvido.

---

#### Método executar

Simula a execução das instruções em cada unidade funcional.  
Avança o contador de ciclos de execução para as instruções ativas e controla o início da execução assim que os operandos ficam prontos.  
Inclui as seguintes lógicas:

- **LOAD**: espera a disponibilidade do registrador base e ausência de _hazards_ com STOREs anteriores; ativa _forwarding_ se o valor estiver pronto.
    
- **STORE**: só inicia quando endereço e valor estão disponíveis.
    
- **ADD/SUB/MUL/DIV**: executam conforme o número de ciclos configurado para cada tipo.
    
- **BNE**: executa em um único ciclo, apenas para comparação e resolução de desvio.
    

---

#### Método escreverResultado_CDB_unico

Gerencia o _write-back_ pelo barramento de dados comum (CDB), garantindo apenas uma escrita por ciclo.  
Adota uma política de prioridade rotativa:

1. ADD/SUB/BNE
    
2. MUL/DIV
    
3. LOAD
    

O método atualiza registradores, limpa estações de reserva e propaga resultados às unidades dependentes via broadcast.  
Instruções `BNE` são resolvidas nesse estágio, sem ocupar o CDB, e determinam se o desvio foi tomado ou não.

---

#### Método escreverResultado_STOREs

Executa o _commit_ das instruções `STORE`, gravando diretamente na memória simulada.  
Essa operação é independente do CDB, permitindo que o armazenamento ocorra paralelamente a outras operações de escrita de resultado.

---

#### Método transmitirResultado

Executa o _broadcast_ dos resultados pelo barramento CDB.  
Atualiza os campos `Vj`, `Vk`, `Vrs`, `fu` de todas as estruturas dependentes que aguardavam aquele resultado, removendo as dependências (`Qj`, `Qk`, `Qrs`).

---

#### Método haTrabalhoPendente

Verifica se ainda existem instruções para emitir, executar ou escrever resultado.  
Retorna falso somente quando todas as unidades funcionais estão livres e todos os resultados foram escritos, marcando o fim da simulação.

---

#### Método Simular

Controla o ciclo principal de simulação.  
A cada iteração, executa na ordem:

1. **Write-Back** (CDB único + commits de STORE)
    
2. **Execução**
    
3. **Emissão**
    

Também verifica a resolução de `BNE`, ajustando o contador de instruções e liberando o bloqueio de emissão.  
O estado do sistema é impresso a cada ciclo, e a simulação termina quando não há mais trabalho pendente.

---

#### Método mostrarEstado

Exibe todas as tabelas da simulação:

- Lista de instruções com ciclos de emissão, execução e escrita
    
- Buffers de `LOAD` e `STORE`
    
- Estações de reserva
    
- Banco de registradores e conteúdo da memória
    
- Log detalhado dos eventos do ciclo anterior
    

Serve para acompanhar visualmente o comportamento do algoritmo e depurar o estado interno do simulador.

---

## Main

A função `main()` realiza a configuração inicial do console (fonte, cor e tamanho), carrega o arquivo de entrada `source.txt`, inicializa o simulador e executa a função `Simular()`.  
Durante a execução, o usuário avança os ciclos pressionando `Enter`.  
Ao final, o estado completo do sistema é exibido, incluindo o número total de ciclos até a conclusão.