#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <iostream>
#include <stdlib.h>
#include <iomanip>
#include <string>
#include <fstream>
#include <conio.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <stdexcept>

using namespace std;

void irPara(short x, short y);

// Registrador simples: nome e valor
struct Registrador {
    string nome;
    int valor;
    Registrador() : nome(""), valor(0) {}
    bool operator==(const Registrador& r) const { return (nome == r.nome); }
};

vector<Registrador> registradores; // F0..Fn
vector<Registrador> memoria;       // memória simulada: par (endereco como string, valor)

// Marcações por instrução (ciclos)
struct StatusInstrucao {
    short emitido;
    short inicioExecucao;
    short fimExecucao;
    short escritaResultado;
    short ciclosRestantesExecucao;
    StatusInstrucao() {
        emitido = inicioExecucao = fimExecucao = escritaResultado = ciclosRestantesExecucao = -1;
    }
};

// “Enums” textuais
struct TiposInstrucao {
public:
    static const string MULT;
    static const string SUBT;
    static const string SOMA;
    static const string DIVI;
    static const string CARREGA;
    static const string BNE;
    static const string ARMAZENA;
};

const string TiposInstrucao::MULT = "MUL";
const string TiposInstrucao::SUBT = "SUB";
const string TiposInstrucao::SOMA = "ADD";
const string TiposInstrucao::DIVI = "DIV";
const string TiposInstrucao::CARREGA = "LOAD";
const string TiposInstrucao::BNE = "BNE";
const string TiposInstrucao::ARMAZENA = "STORE";

struct TipoEstacaoReserva {
    static const string ADIC_SUB;
    static const string MULT_DIV;
};

const string TipoEstacaoReserva::ADIC_SUB = "ADD";
const string TipoEstacaoReserva::MULT_DIV = "MUL";

struct TipoBufferLS {
    static const string CARREGA;
    static const string ARMAZENA;
};

const string TipoBufferLS::CARREGA = "LOAD";
const string TipoBufferLS::ARMAZENA = "STORE";

// Instrução genérica
struct Instrucao {
    string tipoInstrucao;
    string regDestino;   // Rd (R-type)
    string regFonte1;    // Rs (base em LS; op1 em R-type/BNE)
    string regFonte2;    // Rt (op2 em R-type/BNE; valor em STORE; destino em LOAD)
    int offsetImediato;  // imediato L/S; deslocamento em BNE
    StatusInstrucao status;
    Instrucao() : offsetImediato(-1) {
        tipoInstrucao = regDestino = regFonte1 = regFonte2 = "";
    }
};

// Estado do file de registradores (quem vai escrever)
struct EstadoRegistrador {
    string nomeRegistrador;
    string unidadeEscritora; // tag da ER/Buffer
};

// Estação de reserva genérica (usa também para BNE)
struct EstacaoReserva {
    string nome;
    bool ocupado;
    string tipoInstrucao; // "ADD","SUB","MUL","DIV","BNE"
    string valorJ;  // Vj
    string valorK;  // Vk
    string origemJ; // Qj
    string origemK; // Qk
    Instrucao *instrucao;
    EstacaoReserva() : ocupado(false), instrucao(nullptr) {
        valorJ = valorK = origemJ = origemK = "";
    }
};

// Buffers de LOAD/STORE
struct BufferLS {
    string nome;
    bool ocupado;
    string endereco; // representacao textual de Rs+offset
    string origemRs; // produtor do Rs
    string valorRs;  // "R(Fx)" ou valor pronto
    string fu;       // STORE: valor pronto ou produtor; LOAD: vazio
    bool hasForward; // forwarding STORE->LOAD
    string forwardValue;
    Instrucao *instrucao;
    BufferLS() : nome(""), ocupado(false), endereco(""),
                 origemRs(""), valorRs(""), fu(""),
                 hasForward(false), forwardValue(""),
                 instrucao(nullptr) {}
};

// Núcleo do simulador
struct Tomasulo {
    string logEventos;
    int cicloAtual;
    BufferLS *buffersCarregamento;
    BufferLS *buffersArmazenamento;
    EstacaoReserva *estacoesAddSub;
    EstacaoReserva *estacoesMultDiv;
    EstadoRegistrador *estadoRegistradores;

    int numBuffersCarregamento;
    int numBuffersArmazenamento;
    int numEstacoesAddSub;
    int numEstacoesMultDiv;

    int numTotalRegistradores;
    Instrucao* instrucoes;
    int numInstrucoes;

    // latências
    int ciclosLS;
    int ciclosAddSub;
    int ciclosMult;
    int ciclosDiv;

    // arbitragem do CDB (round-robin): 0=ADD/SUB/BNE, 1=MUL/DIV, 2=LOAD
    int cdb_rr = 0;

    // controle de BNE sem especulação
    bool branchPending = false;
    bool branchResolved = false;
    bool branchTaken = false;
    int branchTarget = -1;
    int branchIssuedIndex = -1;

    int eNumero(string s) { return all_of(s.begin(), s.end(), ::isdigit) ? 1 : 0; }

    static bool isRegToken(const string& s) {
        return (s.size() >= 2 && (s[0]=='F' || s[0]=='f') && all_of(s.begin()+1, s.end(), ::isdigit));
    }

    // resolve “string de operando” para valor inteiro
    int obterValorOperando(const string& operando) {
        if (operando.empty()) return 0;
        if (operando.size() > 3 && operando[0] == 'R' && operando[1] == '(' && operando.back() == ')') {
            string regNome = operando.substr(2, operando.size() - 3);
            for (const auto& r : registradores) if (r.nome == regNome) return r.valor;
        }
        try { return stoi(operando); }
        catch (...) { return 0; }
    }

    static bool isValueReadyString(const string& s) {
        if (s.empty()) return false;
        if (s.size() > 3 && s[0]=='R' && s[1]=='(') return false; // ainda é referência a reg
        return true;
    }

    // parse do arquivo de entrada
    void carregarDadosDoArquivo(string nomeArquivo) {
        if (nomeArquivo.empty()) nomeArquivo = "source.txt";
        ifstream leitura(nomeArquivo);
        if (!leitura.is_open()) {
            cout << "O arquivo de entrada nao pode ser aberto: " << nomeArquivo << endl;
            system("pause");
            exit(EXIT_FAILURE);
        }

        string linhaDados;
        while (leitura.peek() != EOF && leitura.peek() == '#') getline(leitura, linhaDados);

        for (int i = 0; i < 4; i++) {
            leitura >> linhaDados;
            if (_stricmp(linhaDados.c_str(), "Add_Sub_Reservation_Stations") == 0) leitura >> this->numEstacoesAddSub;
            else if (_stricmp(linhaDados.c_str(), "Mul_Div_Reservation_Stations") == 0) leitura >> this->numEstacoesMultDiv;
            else if (_stricmp(linhaDados.c_str(), "Load_Buffers") == 0) leitura >> this->numBuffersCarregamento;
            else if (_stricmp(linhaDados.c_str(), "Store_Buffers") == 0) leitura >> this->numBuffersArmazenamento;
        }

        while (leitura.peek() != EOF && leitura.peek() == '#') getline(leitura, linhaDados);

        for (int i = 0; i < 4; i++) {
            leitura >> linhaDados;
            if (_stricmp(linhaDados.c_str(), "Add_Sub_Cycles") == 0) leitura >> this->ciclosAddSub;
            else if (_stricmp(linhaDados.c_str(), "Mul_Cycles") == 0) leitura >> this->ciclosMult;
            else if (_stricmp(linhaDados.c_str(), "Load_Store_Cycles") == 0) leitura >> this->ciclosLS;
            else if (_stricmp(linhaDados.c_str(), "Div_Cycles") == 0) leitura >> this->ciclosDiv;
        }

        // alocação de estruturas
        this->buffersCarregamento = new BufferLS[this->numBuffersCarregamento];
        for (int i = 0; i < numBuffersCarregamento; i++) this->buffersCarregamento[i].nome = TipoBufferLS::CARREGA + to_string(i);

        this->buffersArmazenamento = new BufferLS[this->numBuffersArmazenamento];
        for (int i = 0; i < numBuffersArmazenamento; i++) this->buffersArmazenamento[i].nome = TipoBufferLS::ARMAZENA + to_string(i);

        this->estacoesAddSub = new EstacaoReserva[this->numEstacoesAddSub];
        for (int i = 0; i < this->numEstacoesAddSub; i++) this->estacoesAddSub[i].nome = TipoEstacaoReserva::ADIC_SUB + to_string(i);

        this->estacoesMultDiv = new EstacaoReserva[this->numEstacoesMultDiv];
        for (int i = 0; i < this->numEstacoesMultDiv; i++) this->estacoesMultDiv[i].nome = TipoEstacaoReserva::MULT_DIV + to_string(i);

        while (leitura.peek() != EOF && leitura.peek() == '#') getline(leitura, linhaDados);

        // registradores
        leitura >> linhaDados;
        leitura >> this->numTotalRegistradores;
        this->estadoRegistradores = new EstadoRegistrador[numTotalRegistradores];

        for (int i = 0; i < numTotalRegistradores; i++) {
            this->estadoRegistradores[i].nomeRegistrador = "F" + to_string(i);
            Registrador r; r.nome = "F" + to_string(i); r.valor = 0; registradores.push_back(r);
        }

        while (leitura.peek() != EOF && leitura.peek() == '#') getline(leitura, linhaDados);

        // instruções
        leitura >> this->numInstrucoes;
        this->instrucoes = new Instrucao[this->numInstrucoes];

        for (int i = 0; i < numInstrucoes; i++) {
            string tipo; leitura >> tipo;
            if (_stricmp(tipo.c_str(), TiposInstrucao::SOMA.c_str()) == 0 ||
                _stricmp(tipo.c_str(), TiposInstrucao::SUBT.c_str()) == 0 ||
                _stricmp(tipo.c_str(), TiposInstrucao::MULT.c_str()) == 0 ||
                _stricmp(tipo.c_str(), TiposInstrucao::DIVI.c_str()) == 0) {
                this->instrucoes[i].tipoInstrucao = tipo;
                leitura >> this->instrucoes[i].regDestino;
                leitura >> this->instrucoes[i].regFonte1;
                leitura >> this->instrucoes[i].regFonte2;
            } else if (_stricmp(tipo.c_str(), TiposInstrucao::CARREGA.c_str()) == 0) {
                this->instrucoes[i].tipoInstrucao = TiposInstrucao::CARREGA;
                leitura >> this->instrucoes[i].regFonte2;         // Rt
                leitura >> this->instrucoes[i].offsetImediato;    // offset
                leitura >> this->instrucoes[i].regFonte1;         // Rs
            } else if (_stricmp(tipo.c_str(), TiposInstrucao::ARMAZENA.c_str()) == 0) {
                this->instrucoes[i].tipoInstrucao = TiposInstrucao::ARMAZENA;
                leitura >> this->instrucoes[i].regFonte2;         // Rt
                leitura >> this->instrucoes[i].offsetImediato;
                leitura >> this->instrucoes[i].regFonte1;         // Rs
            } else if (_stricmp(tipo.c_str(), TiposInstrucao::BNE.c_str()) == 0) {
                this->instrucoes[i].tipoInstrucao = TiposInstrucao::BNE;
                leitura >> this->instrucoes[i].regFonte1;         // Rs
                leitura >> this->instrucoes[i].regFonte2;         // Rt
                leitura >> this->instrucoes[i].offsetImediato;    // deslocamento relativo
            } else {
                // ignora
            }
        }
    }

    // busca de recursos livres
    int encontrarBufferLoadLivre() {
        for (int i = 0; i < numBuffersCarregamento; i++) if (!buffersCarregamento[i].ocupado) return i;
        return -1;
    }
    int encontrarBufferStoreLivre() {
        for (int i = 0; i < numBuffersArmazenamento; i++) if (!buffersArmazenamento[i].ocupado) return i;
        return -1;
    }
    int encontrarERAddSubLivre() {
        for (int i = 0; i < numEstacoesAddSub; i++) if (!estacoesAddSub[i].ocupado) return i;
        return -1;
    }
    int encontrarERMultDivLivre() {
        for (int i = 0; i < numEstacoesMultDiv; i++) if (!estacoesMultDiv[i].ocupado) return i;
        return -1;
    }

    // broadcast no “CDB” para destravar dependências
    void transmitirResultado(const string& valor, const string& nomeUnidade) {
        for (int i = 0; i < numBuffersCarregamento; i++) {
            BufferLS& buf = buffersCarregamento[i];
            if (buf.ocupado) {
                if (buf.origemRs == nomeUnidade) { buf.origemRs = ""; buf.valorRs = valor; }
                if (buf.fu == nomeUnidade) { buf.fu = ""; }
            }
        }
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferLS& buf = buffersArmazenamento[i];
            if (buf.ocupado) {
                if (buf.origemRs == nomeUnidade) { buf.origemRs = ""; buf.valorRs = valor; }
                if (buf.fu == nomeUnidade) { buf.fu = valor; }
            }
        }
        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            if (er.origemJ == nomeUnidade) { er.origemJ = ""; er.valorJ = valor; }
            if (er.origemK == nomeUnidade) { er.origemK = ""; er.valorK = valor; }
        }
        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            if (er.origemJ == nomeUnidade) { er.origemJ = ""; er.valorJ = valor; }
            if (er.origemK == nomeUnidade) { er.origemK = ""; er.valorK = valor; }
        }
    }

    // hazards de memória e forwarding para LOAD
    bool checarHazardLoadEForward(BufferLS& loadBuf) {
        loadBuf.hasForward = false;
        loadBuf.forwardValue.clear();

        if (!loadBuf.origemRs.empty()) return true; // ainda depende do Rs

        int loadBase = obterValorOperando(loadBuf.valorRs);
        int loadAddr = loadBase + loadBuf.instrucao->offsetImediato;

        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferLS& st = buffersArmazenamento[i];
            if (!st.ocupado || st.instrucao == nullptr) continue;

            // só STORE mais antigo
            if (st.instrucao->status.emitido == -1) continue;
            if (loadBuf.instrucao->status.emitido == -1) continue;
            if (st.instrucao->status.emitido > loadBuf.instrucao->status.emitido) continue;

            // endereço do STORE ainda desconhecido
            if (!st.origemRs.empty()) return true;

            int stBase = obterValorOperando(st.valorRs);
            int stAddr = stBase + st.instrucao->offsetImediato;

            if (stAddr == loadAddr) {
                if (isValueReadyString(st.fu)) {
                    loadBuf.hasForward = true;
                    loadBuf.forwardValue = st.fu;
                    return false; // libera com forwarding
                } else {
                    return true;  // bloqueia atrás do STORE
                }
            }
        }
        return false; // sem conflito
    }

    // emissão (Issue)
    int emitirInstrucao(int indiceInstrucao) {
        if (indiceInstrucao >= numInstrucoes) return -2;
        if (branchPending) return -1; // segura emissão durante BNE pendente

        Instrucao& instr = instrucoes[indiceInstrucao];
        int indiceBuffer;

        if (instr.tipoInstrucao == TiposInstrucao::CARREGA) {
            indiceBuffer = encontrarBufferLoadLivre();
            if (indiceBuffer == -1) { logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " nao emitida por falta de Buffer de Carga.\n"; return -1; }
            BufferLS& buf = buffersCarregamento[indiceBuffer];

            buf.ocupado = true; buf.instrucao = &instr;
            instr.status.emitido = cicloAtual; instr.status.ciclosRestantesExecucao = ciclosLS;

            int regRsNum = atoi(&instr.regFonte1.c_str()[1]);
            buf.origemRs = estadoRegistradores[regRsNum].unidadeEscritora;
            buf.valorRs = buf.origemRs.empty() ? ("R(" + instr.regFonte1 + ")") : "";
            buf.endereco = instr.regFonte1 + " + " + to_string(instr.offsetImediato);

            int regRetornoNum = atoi(&instr.regFonte2.c_str()[1]);
            estadoRegistradores[regRetornoNum].unidadeEscritora = buf.nome;
            buf.fu = "";
            buf.hasForward = false; buf.forwardValue.clear();

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " emitida para " + buf.nome + " (LOAD).\n";
        }
        else if (instr.tipoInstrucao == TiposInstrucao::ARMAZENA) {
            indiceBuffer = encontrarBufferStoreLivre();
            if (indiceBuffer == -1) { logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " nao emitida por falta de Buffer de Armazenamento.\n"; return -1; }
            BufferLS& buf = buffersArmazenamento[indiceBuffer];

            buf.ocupado = true; buf.instrucao = &instr;
            instr.status.emitido = cicloAtual; instr.status.ciclosRestantesExecucao = ciclosLS;

            int regRsNum = atoi(&instr.regFonte1.c_str()[1]);
            buf.origemRs = estadoRegistradores[regRsNum].unidadeEscritora;
            buf.valorRs = buf.origemRs.empty() ? ("R(" + instr.regFonte1 + ")") : "";
            buf.endereco = instr.regFonte1 + " + " + to_string(instr.offsetImediato);

            int regOrigemNum = atoi(&instr.regFonte2.c_str()[1]);
            buf.fu = estadoRegistradores[regOrigemNum].unidadeEscritora;
            if (buf.fu.empty()) buf.fu = "R(" + instr.regFonte2 + ")";

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " emitida para " + buf.nome + " (STORE).\n";
        }
        else if (instr.tipoInstrucao == TiposInstrucao::SOMA || instr.tipoInstrucao == TiposInstrucao::SUBT) {
            indiceBuffer = encontrarERAddSubLivre();
            if (indiceBuffer == -1) { logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " nao emitida por falta de ER ADD/SUB.\n"; return -1; }
            EstacaoReserva& er = estacoesAddSub[indiceBuffer];

            er.ocupado = true; er.tipoInstrucao = instr.tipoInstrucao; er.instrucao = &instr;
            instr.status.emitido = cicloAtual; instr.status.ciclosRestantesExecucao = ciclosAddSub;

            int regRsNum = atoi(&instr.regFonte1.c_str()[1]);
            er.origemJ = estadoRegistradores[regRsNum].unidadeEscritora;
            er.valorJ = er.origemJ.empty() ? "R(" + instr.regFonte1 + ")" : "";

            int regRtNum = atoi(&instr.regFonte2.c_str()[1]);
            er.origemK = estadoRegistradores[regRtNum].unidadeEscritora;
            er.valorK = er.origemK.empty() ? "R(" + instr.regFonte2 + ")" : "";

            int regRdNum = atoi(&instr.regDestino.c_str()[1]);
            estadoRegistradores[regRdNum].unidadeEscritora = er.nome;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " emitida para " + er.nome + " (ADD/SUB).\n";
        }
        else if (instr.tipoInstrucao == TiposInstrucao::MULT || instr.tipoInstrucao == TiposInstrucao::DIVI) {
            indiceBuffer = encontrarERMultDivLivre();
            if (indiceBuffer == -1) { logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " nao emitida por falta de ER MUL/DIV.\n"; return -1; }
            EstacaoReserva& er = estacoesMultDiv[indiceBuffer];

            er.ocupado = true; er.tipoInstrucao = instr.tipoInstrucao; er.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            instr.status.ciclosRestantesExecucao = (instr.tipoInstrucao == TiposInstrucao::MULT) ? ciclosMult : ciclosDiv;

            int regRsNum = atoi(&instr.regFonte1.c_str()[1]);
            er.origemJ = estadoRegistradores[regRsNum].unidadeEscritora;
            er.valorJ = er.origemJ.empty() ? "R(" + instr.regFonte1 + ")" : "";

            int regRtNum = atoi(&instr.regFonte2.c_str()[1]);
            er.origemK = estadoRegistradores[regRtNum].unidadeEscritora;
            er.valorK = er.origemK.empty() ? "R(" + instr.regFonte2 + ")" : "";

            int regRdNum = atoi(&instr.regDestino.c_str()[1]);
            estadoRegistradores[regRdNum].unidadeEscritora = er.nome;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " emitida para " + er.nome + " (MUL/DIV).\n";
        }
        else if (instr.tipoInstrucao == TiposInstrucao::BNE) {
            // usa ER de ADD para sincronizar dependências
            int idx = encontrarERAddSubLivre();
            if (idx == -1) { logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " nao emitida: ER indisponivel (BNE).\n"; return -1; }
            EstacaoReserva& er = estacoesAddSub[idx];

            er.ocupado = true; er.tipoInstrucao = TiposInstrucao::BNE; er.instrucao = &instr;
            instr.status.emitido = cicloAtual; instr.status.ciclosRestantesExecucao = 1; // custo do compare

            int regRsNum = atoi(&instr.regFonte1.c_str()[1]);
            er.origemJ = estadoRegistradores[regRsNum].unidadeEscritora;
            er.valorJ = er.origemJ.empty() ? "R(" + instr.regFonte1 + ")" : "";

            int regRtNum = atoi(&instr.regFonte2.c_str()[1]);
            er.origemK = estadoRegistradores[regRtNum].unidadeEscritora;
            er.valorK = er.origemK.empty() ? "R(" + instr.regFonte2 + ")" : "";

            // trava emissão até resolver
            branchPending = true;
            branchResolved = false;
            branchIssuedIndex = indiceInstrucao;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) + " emitida para " + er.nome + " (BNE).\n";
        }

        return 0;
    }

    // avanço de execução de todas as FUs
    void executar() {
        // LOAD
        for (int i = 0; i < numBuffersCarregamento; i++) {
            BufferLS& buf = buffersCarregamento[i];
            if (!buf.ocupado) continue;

            bool bloqueia = checarHazardLoadEForward(buf);
            if (bloqueia) continue;

            if (buf.instrucao->status.inicioExecucao == -1) {
                if (buf.instrucao->status.emitido == cicloAtual) continue;
                buf.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> Buffer LOAD " + buf.nome + " iniciou execucao.\n";
            }
            if (buf.instrucao->status.ciclosRestantesExecucao > 0) {
                buf.instrucao->status.ciclosRestantesExecucao--;
                logEventos += "-> Buffer LOAD " + buf.nome + " completou 1 ciclo. Restantes: " + to_string(buf.instrucao->status.ciclosRestantesExecucao) + ".\n";
                if (buf.instrucao->status.ciclosRestantesExecucao == 0) {
                    buf.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> Buffer LOAD " + buf.nome + " completou execucao.\n";
                }
            }
        }

        // STORE
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferLS& buf = buffersArmazenamento[i];
            if (!buf.ocupado) continue;
            if (!buf.origemRs.empty()) continue;
            if (!isValueReadyString(buf.fu)) continue;

            if (buf.instrucao->status.inicioExecucao == -1) {
                if (buf.instrucao->status.emitido == cicloAtual) continue;
                buf.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> Buffer STORE " + buf.nome + " iniciou execucao.\n";
            }
            if (buf.instrucao->status.ciclosRestantesExecucao > 0) {
                buf.instrucao->status.ciclosRestantesExecucao--;
                logEventos += "-> Buffer STORE " + buf.nome + " completou 1 ciclo. Restantes: " + to_string(buf.instrucao->status.ciclosRestantesExecucao) + ".\n";
                if (buf.instrucao->status.ciclosRestantesExecucao == 0) {
                    buf.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> Buffer STORE " + buf.nome + " completou execucao.\n";
                }
            }
        }

        // ADD/SUB/BNE
        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            if (!er.ocupado || !er.origemJ.empty() || !er.origemK.empty()) continue;
            if (er.instrucao->status.inicioExecucao == -1) {
                if (er.instrucao->status.emitido == cicloAtual) continue;
                er.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> ER " + er.nome + " iniciou execucao.\n";
            }
            if (er.instrucao->status.ciclosRestantesExecucao > 0) {
                er.instrucao->status.ciclosRestantesExecucao--;
                logEventos += "-> ER " + er.nome + " completou 1 ciclo. Restantes: " + to_string(er.instrucao->status.ciclosRestantesExecucao) + ".\n";
                if (er.instrucao->status.ciclosRestantesExecucao == 0) {
                    er.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> ER " + er.nome + " completou execucao.\n";
                }
            }
        }

        // MUL/DIV
        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            if (!er.ocupado || !er.origemJ.empty() || !er.origemK.empty()) continue;
            if (er.instrucao->status.inicioExecucao == -1) {
                if (er.instrucao->status.emitido == cicloAtual) continue;
                er.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> ER " + er.nome + " iniciou execucao.\n";
            }
            if (er.instrucao->status.ciclosRestantesExecucao > 0) {
                er.instrucao->status.ciclosRestantesExecucao--;
                logEventos += "-> ER " + er.nome + " completou 1 ciclo. Restantes: " + to_string(er.instrucao->status.ciclosRestantesExecucao) + ".\n";
                if (er.instrucao->status.ciclosRestantesExecucao == 0) {
                    er.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> ER " + er.nome + " completou execucao.\n";
                }
            }
        }
    }

    // um único write-back no CDB por ciclo; STORE escreve direto em memória
    void escreverResultado_CDB_unico() {
        bool escreveu = false;
        for (int turn = 0; turn < 3 && !escreveu; ++turn) {
            int cls = (cdb_rr + turn) % 3;

            if (cls == 0) {
                // ADD/SUB/BNE
                for (int i = 0; i < numEstacoesAddSub; i++) {
                    EstacaoReserva& er = estacoesAddSub[i];
                    if (!er.ocupado || er.instrucao == nullptr) continue;
                    if (er.instrucao->status.ciclosRestantesExecucao != 0) continue;
                    if (er.instrucao->status.fimExecucao == cicloAtual) continue;

                    if (er.tipoInstrucao == TiposInstrucao::BNE) {
                        // resolve desvio
                        int vj = obterValorOperando(er.valorJ);
                        int vk = obterValorOperando(er.valorK);
                        bool taken = (vj != vk);
                        branchResolved = true;
                        branchTaken = taken;
                        if (taken) {
                            int idx = branchIssuedIndex;
                            branchTarget = idx + 1 + er.instrucao->offsetImediato;
                            if (branchTarget < 0) branchTarget = 0;
                            if (branchTarget > numInstrucoes) branchTarget = numInstrucoes;
                        }
                        er.instrucao->status.escritaResultado = cicloAtual;
                        logEventos += "-> BNE resolvido: " + string(taken ? "TAKEN" : "NOT TAKEN") + ".\n";

                        // libera ER
                        er.ocupado = false;
                        er.tipoInstrucao = er.valorJ = er.valorK = er.origemJ = er.origemK = "";
                        er.instrucao = nullptr;

                        cdb_rr = (cls + 1) % 3;
                        escreveu = true;
                        break;
                    } else {
                        // ADD/SUB escreve no CDB
                        er.instrucao->status.escritaResultado = cicloAtual;
                        logEventos += "-> ER " + er.nome + " escreveu resultado no CDB.\n";

                        int valJ = obterValorOperando(er.valorJ);
                        int valK = obterValorOperando(er.valorK);
                        int resultado = 0;
                        if (er.tipoInstrucao == TiposInstrucao::SOMA) resultado = valJ + valK;
                        else if (er.tipoInstrucao == TiposInstrucao::SUBT) resultado = valJ - valK;

                        int regRdNum = atoi(&er.instrucao->regDestino.c_str()[1]);
                        if (estadoRegistradores[regRdNum].unidadeEscritora == er.nome) {
                            estadoRegistradores[regRdNum].unidadeEscritora = "";
                            for (auto& r : registradores) if (r.nome == er.instrucao->regDestino) { r.valor = resultado; break; }
                        }

                        er.ocupado = false; er.tipoInstrucao = er.valorJ = er.valorK = er.origemJ = er.origemK = "";
                        er.instrucao = nullptr;

                        transmitirResultado(to_string(resultado), estacoesAddSub[i].nome);
                        cdb_rr = (cls + 1) % 3;
                        escreveu = true;
                        break;
                    }
                }
            } else if (cls == 1) {
                // MUL/DIV
                for (int i = 0; i < numEstacoesMultDiv; i++) {
                    EstacaoReserva& er = estacoesMultDiv[i];
                    if (!er.ocupado || er.instrucao == nullptr) continue;
                    if (er.instrucao->status.ciclosRestantesExecucao != 0) continue;
                    if (er.instrucao->status.fimExecucao == cicloAtual) continue;

                    er.instrucao->status.escritaResultado = cicloAtual;
                    logEventos += "-> ER " + er.nome + " escreveu resultado no CDB.\n";

                    int valJ = obterValorOperando(er.valorJ);
                    int valK = obterValorOperando(er.valorK);
                    int resultado = 0;
                    if (er.tipoInstrucao == TiposInstrucao::MULT) resultado = valJ * valK;
                    else if (er.tipoInstrucao == TiposInstrucao::DIVI) resultado = (valK == 0) ? 0 : valJ / valK;

                    int regRdNum = atoi(&er.instrucao->regDestino.c_str()[1]);
                    if (estadoRegistradores[regRdNum].unidadeEscritora == er.nome) {
                        estadoRegistradores[regRdNum].unidadeEscritora = "";
                        for (auto& r : registradores) if (r.nome == er.instrucao->regDestino) { r.valor = resultado; break; }
                    }

                    er.ocupado = false; er.tipoInstrucao = er.valorJ = er.valorK = er.origemJ = er.origemK = "";
                    er.instrucao = nullptr;

                    transmitirResultado(to_string(resultado), estacoesMultDiv[i].nome);
                    cdb_rr = (cls + 1) % 3;
                    escreveu = true;
                    break;
                }
            } else {
                // LOAD
                for (int i = 0; i < numBuffersCarregamento; i++) {
                    BufferLS& buf = buffersCarregamento[i];
                    if (!buf.ocupado || buf.instrucao == nullptr) continue;
                    if (buf.instrucao->status.ciclosRestantesExecucao != 0) continue;
                    if (buf.instrucao->status.fimExecucao == cicloAtual) continue;

                    buf.instrucao->status.escritaResultado = cicloAtual;
                    logEventos += "-> Buffer LOAD " + buf.nome + " escreveu resultado no CDB.\n";

                    int valRs = obterValorOperando(buf.valorRs);
                    int endMemoria = valRs + buf.instrucao->offsetImediato;

                    int valorLido = 0;
                    if (buf.hasForward) {
                        valorLido = obterValorOperando(buf.forwardValue);
                    } else {
                        for (const auto& m : memoria) if (m.nome == to_string(endMemoria)) { valorLido = m.valor; break; }
                    }

                    int regRetornoNum = atoi(&buf.instrucao->regFonte2.c_str()[1]);
                    if (estadoRegistradores[regRetornoNum].unidadeEscritora == buf.nome) {
                        estadoRegistradores[regRetornoNum].unidadeEscritora = "";
                        for (auto& r : registradores) if (r.nome == buf.instrucao->regFonte2) { r.valor = valorLido; break; }
                    }

                    buf.ocupado = false; buf.endereco = buf.origemRs = buf.valorRs = buf.fu = "";
                    buf.hasForward = false; buf.forwardValue.clear();
                    buf.instrucao = nullptr;

                    transmitirResultado(to_string(valorLido), buffersCarregamento[i].nome);
                    cdb_rr = (cls + 1) % 3;
                    escreveu = true;
                    break;
                }
            }
        }
    }

    // STORE “commit” direto na memória (fora do CDB)
    void escreverResultado_STOREs() {
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferLS& buf = buffersArmazenamento[i];
            if (!buf.ocupado || buf.instrucao == nullptr) continue;
            if (buf.instrucao->status.ciclosRestantesExecucao != 0) continue;
            if (buf.instrucao->status.fimExecucao == cicloAtual) continue;

            buf.instrucao->status.escritaResultado = cicloAtual;
            logEventos += "-> Buffer STORE " + buf.nome + " escreveu resultado na memoria.\n";

            int valRs = obterValorOperando(buf.valorRs);
            int endMemoria = valRs + buf.instrucao->offsetImediato;
            int valorArmazenar = obterValorOperando(buf.fu);

            Registrador novoItemMem; novoItemMem.nome = to_string(endMemoria); novoItemMem.valor = valorArmazenar;
            bool encontrado = false;
            for (auto& m : memoria) if (m.nome == novoItemMem.nome) { m.valor = novoItemMem.valor; encontrado = true; break; }
            if (!encontrado) memoria.push_back(novoItemMem);

            buf.ocupado = false; buf.endereco = buf.origemRs = buf.valorRs = buf.fu = ""; buf.instrucao = nullptr;
        }
    }

    // verifica se ainda há algo a fazer
    bool haTrabalhoPendente(int proxIndiceInstrucao) const {
        if (proxIndiceInstrucao < numInstrucoes && !branchPending) return true;
        for (int i = 0; i < numBuffersCarregamento; ++i) if (buffersCarregamento[i].ocupado) return true;
        for (int i = 0; i < numBuffersArmazenamento; ++i) if (buffersArmazenamento[i].ocupado) return true;
        for (int i = 0; i < numEstacoesAddSub; ++i) if (estacoesAddSub[i].ocupado) return true;
        for (int i = 0; i < numEstacoesMultDiv; ++i) if (estacoesMultDiv[i].ocupado) return true;
        for (int i = 0; i < numInstrucoes; ++i)
            if (instrucoes[i].status.emitido != -1 && instrucoes[i].status.escritaResultado == -1) return true;
        return false;
    }

    // laço principal: WB -> Execute -> Issue
    void Simular() {
        int proxIndiceInstrucao = 0;
        cicloAtual = 1;

        while (true) {
            irPara(0, 0);
            cout << "Ciclo Atual: " << cicloAtual;
            mostrarEstado();

            cout << "\n\n\n\n\n\nPressione ENTER para o proximo ciclo: ";
            cin.sync();
            cin.get();

            logEventos.clear();

            escreverResultado_CDB_unico();
            escreverResultado_STOREs();
            executar();

            // atualização do PC em caso de BNE resolvido
            if (branchResolved) {
                if (branchTaken) {
                    proxIndiceInstrucao = branchTarget;
                } else {
                    proxIndiceInstrucao = branchIssuedIndex + 1;
                }
                branchPending = false;
                branchResolved = false;
                branchTaken = false;
                branchTarget = -1;
                branchIssuedIndex = -1;
            }

            int resultadoEmissao = -1;
            if (!branchPending && proxIndiceInstrucao < numInstrucoes)
                resultadoEmissao = emitirInstrucao(proxIndiceInstrucao);
            if (resultadoEmissao != -1 && resultadoEmissao != -2) proxIndiceInstrucao++;

            if (!haTrabalhoPendente(proxIndiceInstrucao)) {
                system("cls");
                irPara(0, 0);
                cout << "Ciclo Atual: " << cicloAtual << " (FIM DA SIMULACAO)";
                mostrarEstado();
                cout << "\nSimulacao concluida no Ciclo " << cicloAtual << ".\n";
                break;
            }

            cicloAtual++;
            system("cls");
        }
    }

    // impressão do estado (tabelas)
    void mostrarEstado() {
        int y = 2;
        irPara(2, y); cout << "Instrucoes:";
        irPara(27, y); cout << "Emitido" << " Comeco" << " Fim" << " Escrita";
        irPara(27, y + 1); cout << "__________________________________";

        int offset = 0;
        for (int i = 0; i < numInstrucoes; i++) {
            irPara(2, offset + y + 2);
            string instrStr = to_string(i) + ". " + instrucoes[i].tipoInstrucao + " ";
            if (instrucoes[i].tipoInstrucao == TiposInstrucao::CARREGA || instrucoes[i].tipoInstrucao == TiposInstrucao::ARMAZENA) {
                instrStr += instrucoes[i].regFonte2 + ", " + to_string(instrucoes[i].offsetImediato) + "(" + instrucoes[i].regFonte1 + ")";
            } else if (instrucoes[i].tipoInstrucao == TiposInstrucao::BNE) {
                instrStr += instrucoes[i].regFonte1 + ", " + instrucoes[i].regFonte2 + ", " + to_string(instrucoes[i].offsetImediato);
            } else {
                instrStr += instrucoes[i].regDestino + ", " + instrucoes[i].regFonte1 + ", " + instrucoes[i].regFonte2;
            }
            cout << std::left << setw(24) << instrStr;

            irPara(27, offset + y + 2);
            cout << "|" << std::right << setw(7) << (instrucoes[i].status.emitido == -1 ? "" : to_string(instrucoes[i].status.emitido))
                 << "|" << setw(7) << (instrucoes[i].status.inicioExecucao == -1 ? "" : to_string(instrucoes[i].status.inicioExecucao))
                 << "|" << setw(7) << (instrucoes[i].status.fimExecucao == -1 ? "" : to_string(instrucoes[i].status.fimExecucao))
                 << "|" << setw(9) << (instrucoes[i].status.escritaResultado == -1 ? "" : to_string(instrucoes[i].status.escritaResultado)) << "|";

            offset++;
            irPara(27, offset + y + 2);
            cout << "|_______|_______|_______|_________|";
            offset++;
        }

        int yLS = 2;
        irPara(70, yLS);
        cout << "Load/Store Buffers: Ocupado Endereco Qrs Vrs Qrt/d Vrt/d Rest.";
        yLS++;
        irPara(70 + 2, yLS); cout << "__________________________________________________________";

        for (int i = 0; i < numBuffersCarregamento; i++) {
            yLS++;
            irPara(70, yLS);
            BufferLS& buf = buffersCarregamento[i];
            cout << std::right << setw(10) << buf.nome;
            cout << " |" << setw(7) << (buf.ocupado ? "Sim" : "Nao") << "|" << setw(9) << buf.endereco << "|"
                 << setw(4) << buf.origemRs << "|" << setw(4) << (buf.origemRs.empty() ? buf.valorRs : "") << "|"
                 << setw(6) << buf.fu << "|" << setw(6) << (buf.hasForward ? buf.forwardValue : "") << "|"
                 << setw(5) << (buf.instrucao != nullptr ? to_string(buf.instrucao->status.ciclosRestantesExecucao) : "") << "|";
            yLS++;
            irPara(70 + 11, yLS); cout << "|_______|_________|____|____|______|______|_____|";
        }
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            yLS++;
            irPara(70, yLS);
            BufferLS& buf = buffersArmazenamento[i];
            string fuOutput = "";
            string valueOutput = "";
            if (!buf.fu.empty() && buf.fu[0] != 'R') { valueOutput = buf.fu; }
            else if (!buf.fu.empty() && buf.fu[0] == 'R') { fuOutput = buf.fu; }

            cout << std::right << setw(10) << buf.nome;
            cout << " |" << setw(7) << (buf.ocupado ? "Sim" : "Nao") << "|" << setw(9) << buf.endereco << "|"
                 << setw(4) << buf.origemRs << "|" << setw(4) << (buf.origemRs.empty() ? buf.valorRs : "") << "|"
                 << setw(6) << fuOutput << "|" << setw(6) << valueOutput << "|"
                 << setw(5) << (buf.instrucao != nullptr ? to_string(buf.instrucao->status.ciclosRestantesExecucao) : "") << "|";
            yLS++;
            irPara(70 + 11, yLS); cout << "|_______|_________|____|____|______|______|_____|";
        }

        int yRegs = (offset + y + 2 > yLS ? offset + y + 2 : yLS) + 3;
        irPara(90, yRegs); cout << "Registradores (Valores e Memoria):";
        irPara(90, ++yRegs); cout << " Nome" << "  Valor";
        irPara(90, ++yRegs); cout << "____________";

        size_t regCount = 0;
        for (const auto& r : registradores) {
            if (eNumero(r.nome) == 0) {
                yRegs++;
                irPara(90, yRegs);
                cout << "| " << std::left << setw(4) << r.nome << "| " << std::right << setw(5) << r.valor << "|";
                regCount++;
            }
        }
        if (regCount > 0) { yRegs++; irPara(90, yRegs); cout << "|_____|_______|"; }

        yRegs++;
        irPara(90, ++yRegs); cout << "Memoria";
        irPara(90, ++yRegs); cout << " End." << "  Valor";
        irPara(90, ++yRegs); cout << "____________";
        for (const auto& m : memoria) {
            yRegs++;
            irPara(90, yRegs);
            cout << "| " << std::left << setw(4) << m.nome << "| " << std::right << setw(5) << m.valor << "|";
        }
        if (memoria.size() > 0) { yRegs++; irPara(90, yRegs); cout << "|____|_______|"; }

        int yER = (yRegs > yLS ? yRegs : yLS) + 3;
        irPara(4, yER); cout << "Estacoes de Reserva (ERs):";
        yER++;
        irPara(21, yER); cout << " Nome  Ocupado  Op  ValorJ  ValorK  OrigemJ  OrigemK  Rest.";
        yER++;
        irPara(21 + 7, yER); cout << "_________________________________________________________";

        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            yER++;
            irPara(19, yER);
            cout << std::right << setw(7) << er.nome << " |" << setw(7) << (er.ocupado ? "Sim" : "Nao") << "|" << setw(4) << er.tipoInstrucao << "|"
                 << setw(7) << (er.origemJ.empty() ? er.valorJ : "") << "|" << setw(7) << (er.origemK.empty() ? er.valorK : "") << "|" << setw(8) << er.origemJ << "|" << setw(8) << er.origemK << "|"
                 << setw(5) << (er.instrucao != nullptr ? to_string(er.instrucao->status.ciclosRestantesExecucao) : "") << "|";
            yER++;
            irPara(19 + 7 + 1, yER); cout << "|_______|____|_______|_______|________|________|_____|";
        }
        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            yER++;
            irPara(19, yER);
            cout << std::right << setw(7) << er.nome << " |" << setw(7) << (er.ocupado ? "Sim" : "Nao") << "|" << setw(4) << er.tipoInstrucao << "|"
                 << setw(7) << (er.origemJ.empty() ? er.valorJ : "") << "|" << setw(7) << (er.origemK.empty() ? er.valorK : "") << "|" << setw(8) << er.origemJ << "|" << setw(8) << er.origemK << "|"
                 << setw(5) << (er.instrucao != nullptr ? to_string(er.instrucao->status.ciclosRestantesExecucao) : "") << "|";
            yER++;
            irPara(19 + 7 + 1, yER); cout << "|_______|____|_______|_______|________|________|_____|";
        }

        int yStatusReg = yER + 3;
        irPara(20, yStatusReg); cout << "Estado dos Registradores (Unidade Escritora - Q.i):";
        yStatusReg++;

        int xPos = 20;
        for (int i = 0; i < numTotalRegistradores; i++) {
            irPara(xPos, yStatusReg); cout << std::right << setw(5) << estadoRegistradores[i].nomeRegistrador;
            irPara(xPos, yStatusReg + 1); cout << "______";
            irPara(xPos, yStatusReg + 2); cout << "|" << setw(4) << estadoRegistradores[i].unidadeEscritora << "|";
            irPara(xPos, yStatusReg + 3); cout << "|______|";
            xPos += 8;
        }

        irPara(2, yStatusReg + 5);
        cout << "\n\nEventos do Ciclo " << cicloAtual - 1 << " (Log): \n" << logEventos;
    }

    Tomasulo() {}
};

int main(int /*argc*/, char* /*argv*/[]) {
    // aumenta a fonte para caber melhor a “UI” de console
    CONSOLE_FONT_INFOEX info{};
    info.cbSize = sizeof(info);
    info.nFont = 0;
    info.dwFontSize.X = 0;
    info.dwFontSize.Y = 18;
    info.FontFamily = FF_DONTCARE;
    info.FontWeight = FW_NORMAL;
    wcscpy_s(info.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &info);

    // cor de fundo/primeiro plano
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
    system("cls");

    Tomasulo simulador;
    simulador.carregarDadosDoArquivo("./source.txt");
    simulador.Simular();
    return 0;
}

// posiciona cursor no console
void irPara(short x, short y) {
    COORD c = { x, y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), c);
}
