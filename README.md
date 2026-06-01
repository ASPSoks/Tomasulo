# Simulador Superescalar do Algoritmo de Tomasulo com ROB

**Disciplina:** Arquitetura de Computadores III  
**Linguagem:** C++17  
**Referência:** Hennessy & Patterson — _Arquitetura de Computadores: Uma Abordagem Quantitativa_  
**Grupo:** André Scianni, Davi Caetano, Enzo Moraes e Lucas Pereira

---

## Visão Geral

Este projeto implementa um simulador do algoritmo de Tomasulo com suporte a execução superescalar e reordenação de instruções por meio de um **Reorder Buffer (ROB)**.

### Principais funcionalidades

- Emissão (_issue_) de múltiplas instruções por ciclo
- Execução fora de ordem (_out-of-order execution_)
- Estações de reserva distribuídas por tipo de unidade funcional
- Reordenação de instruções utilizando ROB
- Renomeação implícita de registradores por meio de tags do ROB
- Barramento comum de dados (_Common Data Bus – CDB_) configurável
- Múltiplas unidades funcionais independentes
- Suporte a desvios condicionais com:
    - **Stall**
    - **Predict Not Taken**
- Tratamento de dependências de memória entre LOAD e STORE
- _Store forwarding_

---

## Compilação

```
g++ -std=c++17 -O2 -o tomasulo tomasulo.cpp
```

---

## Execução

```
./tomasulo <arquivo_entrada> [--auto] [--max N]
```

### Argumentos

|Argumento|Descrição|
|---|---|
|`arquivo_entrada`|Arquivo de entrada contendo configuração e instruções|
|`--auto`|Executa todos os ciclos automaticamente|
|`--max N`|Limita a execução a N ciclos|

### Exemplos

```
./tomasulo source.txt
```

Execução passo a passo.

```
./tomasulo source.txt --auto
```

Execução automática até o término.

```
./tomasulo source.txt --auto --max 50
```

Execução automática limitada a 50 ciclos.

---

## Formato do Arquivo de Entrada

Comentários são iniciados com `#`.

### 1. Configuração da Arquitetura

```
Add_Sub_Reservation_Stations 3
Mul_Div_Reservation_Stations 2
Load_Buffers 2
Store_Buffers 2

Add_Sub_Cycles 2
Mul_Cycles 4
Div_Cycles 8
Load_Store_Cycles 2
Branch_Cycles 1

Issue_Width 2
CDB_Width 2
Commit_Width 2

ROB_Entries 16

Add_Sub_Functional_Units 2
Mul_Div_Functional_Units 1
Load_Store_Functional_Units 1
Branch_Functional_Units 1

Branch_Mode Stall
```

### 2. Quantidade de Registradores

```
Registers 16
```

### 3. Inicialização de Registradores e Memória

```
F0 10
F1 20

Memory 100 5
M104 12
```

### 4. Quantidade de Instruções

```
10
```

### 5. Programa

```
LOAD F1 0 F0
ADD F2 F1 F3
MUL F4 F2 F5
STORE F4 4 F0
```

---

## Parâmetros Configuráveis

|   |   |
|---|---|
|Parâmetro|Descrição|
|`Add_Sub_Reservation_Stations`|Número de estações para ADD e SUB|
|`Mul_Div_Reservation_Stations`|Número de estações para MUL e DIV|
|`Load_Buffers`|Quantidade de buffers de LOAD|
|`Store_Buffers`|Quantidade de buffers de STORE|
|`Add_Sub_Cycles`|Latência de ADD e SUB|
|`Mul_Cycles`|Latência de MUL|
|`Div_Cycles`|Latência de DIV|
|`Load_Store_Cycles`|Latência de LOAD e STORE|
|`Branch_Cycles`|Latência de desvios|
|`Issue_Width`|Instruções emitidas por ciclo|
|`CDB_Width`|Resultados escritos no CDB por ciclo|
|`Commit_Width`|Commits realizados por ciclo|
|`ROB_Entries`|Número de entradas do ROB|
|`Add_Sub_Functional_Units`|UFs para ADD e SUB|
|`Mul_Div_Functional_Units`|UFs para MUL e DIV|
|`Load_Store_Functional_Units`|UFs para LOAD e STORE|
|`Branch_Functional_Units`|UFs para desvios|
|`Branch_Mode`|`Stall` ou `Predict_Not_Taken`|

---

## Conjunto de Instruções

|   |   |   |
|---|---|---|
|Instrução|Sintaxe|Operação|
|ADD|`ADD Fd Fs Ft`|`Fd = Fs + Ft`|
|SUB|`SUB Fd Fs Ft`|`Fd = Fs - Ft`|
|MUL|`MUL Fd Fs Ft`|`Fd = Fs * Ft`|
|DIV|`DIV Fd Fs Ft`|`Fd = Fs / Ft`|
|LOAD|`LOAD Fd offset Fbase`|`Fd = Mem[Fbase + offset]`|
|STORE|`STORE Fs offset Fbase`|`Mem[Fbase + offset] = Fs`|
|BEQ|`BEQ Fs Ft offset`|Desvia se `Fs == Ft`|
|BNE|`BNE Fs Ft offset`|Desvia se `Fs != Ft`|

Os registradores podem ser especificados como:

```
F0
R0
$R0
```

---

## Organização do Ciclo

Em cada ciclo, o simulador executa as seguintes etapas:

```
1. escreverCDB()
2. executar()
3. commitROB()
4. emitirSuperescalar()
```

### Justificativa

A escrita no CDB ocorre antes das demais etapas para que resultados produzidos no ciclo anterior estejam disponíveis para novas instruções no ciclo corrente.

Uma instrução emitida no ciclo N não pode iniciar execução no mesmo ciclo.

---

## Renomeação de Registradores

O simulador utiliza uma **Register Alias Table (RAT)** associada ao ROB.

Quando uma instrução produz um registrador de destino:

```
RAT[Fd] = ROBx
```

As instruções consumidoras passam a depender da tag do ROB em vez do valor arquitetural.

Esse mecanismo elimina:

- Dependências WAR (_Write After Read_)
- Dependências WAW (_Write After Write_)

O valor arquitetural é atualizado apenas durante o commit.

---

## Tratamento de Branches

### Stall

Ao emitir um branch, novas instruções deixam de ser emitidas até sua resolução.

Características:

- Sem especulação
- Implementação simples
- Menor paralelismo

### Predict Not Taken

O simulador assume que o branch não será tomado.

Se a predição estiver incorreta:

- As instruções especulativas são descartadas
- O ROB é restaurado
- A RAT retorna ao estado salvo
- O PC é corrigido

---

## Dependências de Memória

Antes da execução de um LOAD:

1. O ROB é percorrido procurando STOREs anteriores.
2. Se existir STORE com endereço desconhecido, o LOAD aguarda.
3. Se existir STORE para o mesmo endereço com valor disponível, ocorre _store forwarding_.
4. Caso contrário, a leitura é feita diretamente da memória.

Os STOREs atualizam a memória apenas no commit.

---

## Informações Exibidas por Ciclo

O simulador apresenta:

1. Estado geral da execução
2. Tabela de instruções dinâmicas
3. Estações de reserva
4. Buffers de LOAD/STORE
5. Reorder Buffer
6. Banco de registradores e RAT
7. Estado da memória
8. Log de eventos do ciclo

---

## Estruturas de Dados Principais

|   |   |
|---|---|
|Estrutura|Função|
|`RSEntry`|Estação de reserva|
|`LSEntry`|Buffer de LOAD/STORE|
|`FU`|Unidade funcional|
|`ROBEntry`|Entrada do ROB|
|`DynInstr`|Histórico de instruções dinâmicas|
|`PendingWB`|Resultado aguardando escrita no CDB|

---

## Estrutura do Código

```
main()
 ├── carregarArquivo()
 ├── simular()
 │
 ├── escreverCDB()
 ├── executar()
 │   ├── assignRS()
 │   ├── assignLS()
 │   └── advanceFUGroup()
 │
 ├── commitROB()
 └── emitirSuperescalar()
```

---

## Métricas Finais

Ao término da simulação são reportadas:

- Número total de ciclos
- CPI (Cycles Per Instruction)
- Instruções emitidas
- Instruções commitadas
- Instruções descartadas por flush
- Slots de issue desperdiçados
- Ciclos sem emissão
- Estado final dos registradores
- Estado final da memória

---

## Exemplo de Branch

```
Branch_Mode Predict_Not_Taken

BEQ F0 F1 2
BNE F2 F3 -4
```

O primeiro branch avança duas instruções caso `F0 == F1`.

O segundo branch implementa um laço, retornando quatro posições caso `F2 != F3`.