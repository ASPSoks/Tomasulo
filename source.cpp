#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cctype>

using namespace std;

void irPara(short x, short y);

// Registrador simples: nome e valor
struct Registrador {
    string nome;
    int valor;
    Registrador() : nome(""), valor(0) {}
    bool operator==(const Registrador& r) const { return nome == r.nome; }
};

vector<Registrador> registradores; // F0..Fn
vector<Registrador> memoria;       // memória simulada: par (endereco como string, valor)

// Funções auxiliares globais
int obterValorRegistrador(const string& nomeReg) {
    for (const auto& r : registradores)
        if (r.nome == nomeReg) return r.valor;
    return 0;
}

int lerMemoria(int endereco) {
    string k = to_string(endereco);
    for (const auto& m : memoria)
        if (m.nome == k) return m.valor;
    return 0;
}

void escreverMemoria(int endereco, int valor) {
    string k = to_string(endereco);
    for (auto& m : memoria) {
        if (m.nome == k) {
            m.valor = valor;
            return;
        }
    }
    Registrador r;
    r.nome = k;
    r.valor = valor;
    memoria.push_back(r);
}

// Marcações por instrução (ciclos)
struct StatusInstrucao {
    int emitido;
    int inicioExecucao;
    int fimExecucao;
    int escritaResultado;
    int ciclosRestantesExecucao;
    StatusInstrucao() {
        emitido = inicioExecucao = fimExecucao = escritaResultado = ciclosRestantesExecucao = -1;
    }
};

// “Enums” textuais
struct TiposInstrucao {
    static const string MULT;
    static const string SUBT;
    static const string SOMA;
    static const string DIVI;
    static const string CARREGA;
    static const string BNE;
    static const string ARMAZENA;
};

const string TiposInstrucao::MULT     = "MUL";
const string TiposInstrucao::SUBT     = "SUB";
const string TiposInstrucao::SOMA     = "ADD";
const string TiposInstrucao::DIVI     = "DIV";
const string TiposInstrucao::CARREGA  = "LOAD";
const string TiposInstrucao::BNE      = "BNE";
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
    int valorJ;  // Vj (literal quando pronto)
    int valorK;  // Vk (literal quando pronto)
    string origemJ; // Qj (tag)
    string origemK; // Qk (tag)
    int destReg;    // índice do registrador destino (para R-type)
    Instrucao *instrucao;
    int ciclosRestantes;
    EstacaoReserva()
        : nome(""), ocupado(false), tipoInstrucao(""),
          valorJ(0), valorK(0), origemJ(""), origemK(""),
          destReg(-1), instrucao(nullptr), ciclosRestantes(-1) {}
};

// Buffers de LOAD
struct BufferLoad {
    string nome;
    bool ocupado;
    int baseVal;      // valor numérico quando pronto
    string origemBase; // tag do produtor da base
    int offset;
    int destReg;      // índice do registrador destino
    int ciclosRestantes;
    bool resultReady;
    int resultado;
    bool hasForward;
    int forwardVal;
    Instrucao* instrucao;
    BufferLoad()
        : nome(""), ocupado(false), baseVal(0), origemBase(""),
          offset(0), destReg(-1), ciclosRestantes(-1),
          resultReady(false), resultado(0),
          hasForward(false), forwardVal(0),
          instrucao(nullptr) {}
};

// Buffers de STORE
struct BufferStore {
    string nome;
    bool ocupado;
    int baseVal;
    string origemBase;
    int offset;
    int value;
    string origemVal;
    int ciclosRestantes;
    Instrucao* instrucao;
    BufferStore()
        : nome(""), ocupado(false), baseVal(0), origemBase(""),
          offset(0), value(0), origemVal(""),
          ciclosRestantes(-1), instrucao(nullptr) {}
};

// Núcleo do simulador
struct Tomasulo {
    string logEventos;
    int cicloAtual = 0;

    BufferLoad*  buffersCarregamento   = nullptr;
    BufferStore* buffersArmazenamento  = nullptr;
    EstacaoReserva* estacoesAddSub     = nullptr;
    EstacaoReserva* estacoesMultDiv    = nullptr;
    EstadoRegistrador* estadoRegistradores = nullptr;

    int numBuffersCarregamento = 0;
    int numBuffersArmazenamento = 0;
    int numEstacoesAddSub = 0;
    int numEstacoesMultDiv = 0;

    int numTotalRegistradores = 0;
    Instrucao* instrucoes = nullptr;
    int numInstrucoes = 0;

    int ciclosLS = 1;
    int ciclosAddSub = 1;
    int ciclosMult = 1;
    int ciclosDiv = 1;

    int cdb_rr = 0; // 0=ADD/SUB/BNE, 1=MUL/DIV, 2=LOAD

    bool branchPending = false;
    bool branchResolved = false;
    bool branchTaken = false;
    int branchTarget = -1;
    int branchIssuedIndex = -1;

    int eNumero(const string& s) const {
        return all_of(s.begin(), s.end(),
                      [](unsigned char c){ return std::isdigit(c); }) ? 1 : 0;
    }

    static int obterValorOperando(const string& operando) {
        if (operando.empty()) return 0;
        try { return stoi(operando); }
        catch (...) { return 0; }
    }

    static bool isValueReadyString(const string& s) {
        if (s.empty()) return false;
        size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (i >= s.size()) return false;
        return all_of(s.begin() + i, s.end(),
                      [](unsigned char c){ return std::isdigit(c); });
    }

    int regIndex(const string& r) const {
        if (r.size() < 2 || (r[0] != 'F' && r[0] != 'f')) return -1;
        int n = atoi(r.c_str() + 1);
        if (n < 0 || n >= numTotalRegistradores) return -1;
        return n;
    }

    void carregarDadosDoArquivo(const string& nomeArquivo) {
        ifstream leitura(nomeArquivo);
        if (!leitura.is_open()) {
            cout << "O arquivo de entrada nao pode ser aberto: " << nomeArquivo << endl;
            system("pause");
            exit(EXIT_FAILURE);
        }

        string linhaDados;

        while (leitura.peek() != EOF && leitura.peek() == '#')
            getline(leitura, linhaDados);

        for (int i = 0; i < 4; i++) {
            leitura >> linhaDados;
            if (linhaDados == "Add_Sub_Reservation_Stations")
                leitura >> numEstacoesAddSub;
            else if (linhaDados == "Mul_Div_Reservation_Stations")
                leitura >> numEstacoesMultDiv;
            else if (linhaDados == "Load_Buffers")
                leitura >> numBuffersCarregamento;
            else if (linhaDados == "Store_Buffers")
                leitura >> numBuffersArmazenamento;
        }

        while (leitura.peek() != EOF && leitura.peek() == '#')
            getline(leitura, linhaDados);

        for (int i = 0; i < 4; i++) {
            leitura >> linhaDados;
            if (linhaDados == "Add_Sub_Cycles")
                leitura >> ciclosAddSub;
            else if (linhaDados == "Mul_Cycles")
                leitura >> ciclosMult;
            else if (linhaDados == "Load_Store_Cycles")
                leitura >> ciclosLS;
            else if (linhaDados == "Div_Cycles")
                leitura >> ciclosDiv;
        }

        ciclosAddSub = max(1, ciclosAddSub);
        ciclosMult   = max(1, ciclosMult);
        ciclosDiv    = max(1, ciclosDiv);
        ciclosLS     = max(1, ciclosLS);

        buffersCarregamento = new BufferLoad[numBuffersCarregamento];
        for (int i = 0; i < numBuffersCarregamento; i++)
            buffersCarregamento[i].nome = TipoBufferLS::CARREGA + to_string(i);

        buffersArmazenamento = new BufferStore[numBuffersArmazenamento];
        for (int i = 0; i < numBuffersArmazenamento; i++)
            buffersArmazenamento[i].nome = TipoBufferLS::ARMAZENA + to_string(i);

        estacoesAddSub = new EstacaoReserva[numEstacoesAddSub];
        for (int i = 0; i < numEstacoesAddSub; i++)
            estacoesAddSub[i].nome = TipoEstacaoReserva::ADIC_SUB + to_string(i);

        estacoesMultDiv = new EstacaoReserva[numEstacoesMultDiv];
        for (int i = 0; i < numEstacoesMultDiv; i++)
            estacoesMultDiv[i].nome = TipoEstacaoReserva::MULT_DIV + to_string(i);

        while (leitura.peek() != EOF && leitura.peek() == '#')
            getline(leitura, linhaDados);

        leitura >> linhaDados; // "Registers"
        leitura >> numTotalRegistradores;
        estadoRegistradores = new EstadoRegistrador[numTotalRegistradores];

        for (int i = 0; i < numTotalRegistradores; i++) {
            estadoRegistradores[i].nomeRegistrador = "F" + to_string(i);
            estadoRegistradores[i].unidadeEscritora.clear();
            Registrador r;
            r.nome = "F" + to_string(i);
            r.valor = 0;
            registradores.push_back(r);
        }

        while (leitura.peek() != EOF && leitura.peek() == '#')
            getline(leitura, linhaDados);

        string token;
        if (!(leitura >> token)) {
            cout << "Erro ao ler arquivo apos declaracao de registradores.\n";
            exit(EXIT_FAILURE);
        }

        if (!token.empty() && (token[0] == 'F' || token[0] == 'f')) {
            while (true) {
                string regName = token;
                int val;
                if (!(leitura >> val)) {
                    cout << "Erro ao ler valor inicial de " << regName << ".\n";
                    exit(EXIT_FAILURE);
                }

                int idx = regIndex(regName);
                if (idx >= 0 && idx < numTotalRegistradores) {
                    registradores[idx].valor = val;
                } else {
                    cout << "Registrador invalido na inicializacao: " << regName << endl;
                    exit(EXIT_FAILURE);
                }

                if (!(leitura >> token)) {
                    cout << "Fim inesperado ao procurar numero de instrucoes.\n";
                    exit(EXIT_FAILURE);
                }

                if (!(token[0] == 'F' || token[0] == 'f')) {
                    try {
                        numInstrucoes = stoi(token);
                    } catch (...) {
                        cout << "Token inesperado onde se esperava numero de instrucoes: " << token << endl;
                        exit(EXIT_FAILURE);
                    }
                    break;
                }
            }
        } else {
            try {
                numInstrucoes = stoi(token);
            } catch (...) {
                cout << "Token inesperado onde se esperava numero de instrucoes: " << token << endl;
                exit(EXIT_FAILURE);
            }
        }

        instrucoes = new Instrucao[numInstrucoes];

        for (int i = 0; i < numInstrucoes; i++) {
            string tipo; leitura >> tipo;
            if (tipo == TiposInstrucao::SOMA ||
                tipo == TiposInstrucao::SUBT ||
                tipo == TiposInstrucao::MULT ||
                tipo == TiposInstrucao::DIVI) {
                instrucoes[i].tipoInstrucao = tipo;
                leitura >> instrucoes[i].regDestino;
                leitura >> instrucoes[i].regFonte1;
                leitura >> instrucoes[i].regFonte2;
            } else if (tipo == TiposInstrucao::CARREGA) {
                instrucoes[i].tipoInstrucao = TiposInstrucao::CARREGA;
                leitura >> instrucoes[i].regFonte2;      // Rt
                leitura >> instrucoes[i].offsetImediato; // offset
                leitura >> instrucoes[i].regFonte1;      // Rs
            } else if (tipo == TiposInstrucao::ARMAZENA) {
                instrucoes[i].tipoInstrucao = TiposInstrucao::ARMAZENA;
                leitura >> instrucoes[i].regFonte2;      // Rt (valor)
                leitura >> instrucoes[i].offsetImediato;
                leitura >> instrucoes[i].regFonte1;      // Rs (base)
            } else if (tipo == TiposInstrucao::BNE) {
                instrucoes[i].tipoInstrucao = TiposInstrucao::BNE;
                leitura >> instrucoes[i].regFonte1;      // Rs
                leitura >> instrucoes[i].regFonte2;      // Rt
                leitura >> instrucoes[i].offsetImediato; // deslocamento
            } else {
                // ignora tokens desconhecidos
            }
        }
    }

    int encontrarBufferLoadLivre() {
        for (int i = 0; i < numBuffersCarregamento; i++)
            if (!buffersCarregamento[i].ocupado) return i;
        return -1;
    }

    int encontrarBufferStoreLivre() {
        for (int i = 0; i < numBuffersArmazenamento; i++)
            if (!buffersArmazenamento[i].ocupado) return i;
        return -1;
    }

    int encontrarERAddSubLivre() {
        for (int i = 0; i < numEstacoesAddSub; i++)
            if (!estacoesAddSub[i].ocupado) return i;
        return -1;
    }

    int encontrarERMultDivLivre() {
        for (int i = 0; i < numEstacoesMultDiv; i++)
            if (!estacoesMultDiv[i].ocupado) return i;
        return -1;
    }

    void transmitirResultado(int valor, const string& nomeUnidade) {
        for (int i = 0; i < numBuffersCarregamento; i++) {
            BufferLoad& lb = buffersCarregamento[i];
            if (!lb.ocupado) continue;
            if (lb.origemBase == nomeUnidade) {
                lb.origemBase.clear();
                lb.baseVal = valor;
            }
        }
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferStore& sb = buffersArmazenamento[i];
            if (!sb.ocupado) continue;
            if (sb.origemBase == nomeUnidade) {
                sb.origemBase.clear();
                sb.baseVal = valor;
            }
            if (sb.origemVal == nomeUnidade) {
                sb.origemVal.clear();
                sb.value = valor;
            }
        }
        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            if (!er.ocupado) continue;
            if (er.origemJ == nomeUnidade) {
                er.origemJ.clear();
                er.valorJ = valor;
            }
            if (er.origemK == nomeUnidade) {
                er.origemK.clear();
                er.valorK = valor;
            }
        }
        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            if (!er.ocupado) continue;
            if (er.origemJ == nomeUnidade) {
                er.origemJ.clear();
                er.valorJ = valor;
            }
            if (er.origemK == nomeUnidade) {
                er.origemK.clear();
                er.valorK = valor;
            }
        }
    }

    bool checarHazardLoadEForward(BufferLoad& loadBuf) {
        loadBuf.hasForward = false;
        loadBuf.forwardVal = 0;

        if (!loadBuf.origemBase.empty()) return true;

        int loadAddr = loadBuf.baseVal + loadBuf.instrucao->offsetImediato;

        bool existeStoreAntigoMesmoEndNaoPronto = false;
        int melhorEmit = -1;
        int melhorVal = 0;

        for (int i = 0; i < numBuffersArmazenamento; ++i) {
            BufferStore& st = buffersArmazenamento[i];
            if (!st.ocupado || !st.instrucao) continue;

            int emitSt = st.instrucao->status.emitido;
            int emitLd = loadBuf.instrucao->status.emitido;
            if (emitSt == -1 || emitSt > emitLd) continue;

            if (!st.origemBase.empty()) {
                existeStoreAntigoMesmoEndNaoPronto = true;
                continue;
            }

            int stAddr = st.baseVal + st.instrucao->offsetImediato;
            if (stAddr == loadAddr) {
                if (st.origemVal.empty()) {
                    if (emitSt > melhorEmit) {
                        melhorEmit = emitSt;
                        melhorVal = st.value;
                    }
                } else {
                    existeStoreAntigoMesmoEndNaoPronto = true;
                }
            }
        }

        if (existeStoreAntigoMesmoEndNaoPronto) return true;
        if (melhorEmit != -1) {
            loadBuf.hasForward = true;
            loadBuf.forwardVal = melhorVal;
        }
        return false;
    }

    int emitirInstrucao(int indiceInstrucao) {
        if (indiceInstrucao >= numInstrucoes) return -2;
        if (branchPending) return -1;

        Instrucao& instr = instrucoes[indiceInstrucao];

        if (instr.tipoInstrucao == TiposInstrucao::CARREGA) {
            int idx = encontrarBufferLoadLivre();
            if (idx == -1) {
                logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                              " nao emitida (sem Buffer LOAD).\n";
                return -1;
            }
            BufferLoad& lb = buffersCarregamento[idx];
            lb.ocupado = true;
            lb.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            instr.status.ciclosRestantesExecucao = ciclosLS;
            lb.ciclosRestantes = ciclosLS;

            int rsIdx = regIndex(instr.regFonte1);
            if (rsIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte1);
            lb.origemBase = estadoRegistradores[rsIdx].unidadeEscritora;
            if (lb.origemBase.empty())
                lb.baseVal = obterValorRegistrador(instr.regFonte1);
            lb.offset = instr.offsetImediato;

            int rdIdx = regIndex(instr.regFonte2);
            if (rdIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte2);
            lb.destReg = rdIdx;
            estadoRegistradores[rdIdx].unidadeEscritora = lb.nome;

            lb.resultReady = false;
            lb.hasForward = false;
            lb.forwardVal = 0;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                          " emitida para " + lb.nome + " (LOAD).\n";
            return 0;
        } else if (instr.tipoInstrucao == TiposInstrucao::ARMAZENA) {
            int idx = encontrarBufferStoreLivre();
            if (idx == -1) {
                logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                              " nao emitida (sem Buffer STORE).\n";
                return -1;
            }
            BufferStore& sb = buffersArmazenamento[idx];
            sb.ocupado = true;
            sb.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            instr.status.ciclosRestantesExecucao = ciclosLS;
            sb.ciclosRestantes = ciclosLS;

            int rsIdx = regIndex(instr.regFonte1);
            if (rsIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte1);
            sb.origemBase = estadoRegistradores[rsIdx].unidadeEscritora;
            if (sb.origemBase.empty())
                sb.baseVal = obterValorRegistrador(instr.regFonte1);
            sb.offset = instr.offsetImediato;

            int rtIdx = regIndex(instr.regFonte2);
            if (rtIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte2);
            sb.origemVal = estadoRegistradores[rtIdx].unidadeEscritora;
            if (sb.origemVal.empty())
                sb.value = obterValorRegistrador(instr.regFonte2);

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                          " emitida para " + sb.nome + " (STORE).\n";
            return 0;
        } else if (instr.tipoInstrucao == TiposInstrucao::SOMA ||
                   instr.tipoInstrucao == TiposInstrucao::SUBT) {
            int idx = encontrarERAddSubLivre();
            if (idx == -1) {
                logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                              " nao emitida (sem ER ADD/SUB).\n";
                return -1;
            }
            EstacaoReserva& er = estacoesAddSub[idx];
            er.ocupado = true;
            er.tipoInstrucao = instr.tipoInstrucao;
            er.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            instr.status.ciclosRestantesExecucao = ciclosAddSub;
            er.ciclosRestantes = ciclosAddSub;

            int rsIdx = regIndex(instr.regFonte1);
            if (rsIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte1);
            er.origemJ = estadoRegistradores[rsIdx].unidadeEscritora;
            if (er.origemJ.empty())
                er.valorJ = obterValorRegistrador(instr.regFonte1);

            int rtIdx = regIndex(instr.regFonte2);
            if (rtIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte2);
            er.origemK = estadoRegistradores[rtIdx].unidadeEscritora;
            if (er.origemK.empty())
                er.valorK = obterValorRegistrador(instr.regFonte2);

            int rdIdx = regIndex(instr.regDestino);
            if (rdIdx < 0) throw runtime_error("Registrador invalido: " + instr.regDestino);
            er.destReg = rdIdx;
            estadoRegistradores[rdIdx].unidadeEscritora = er.nome;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                          " emitida para " + er.nome + " (ADD/SUB).\n";
            return 0;
        } else if (instr.tipoInstrucao == TiposInstrucao::MULT ||
                   instr.tipoInstrucao == TiposInstrucao::DIVI) {
            int idx = encontrarERMultDivLivre();
            if (idx == -1) {
                logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                              " nao emitida (sem ER MUL/DIV).\n";
                return -1;
            }
            EstacaoReserva& er = estacoesMultDiv[idx];
            er.ocupado = true;
            er.tipoInstrucao = instr.tipoInstrucao;
            er.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            int lat = (instr.tipoInstrucao == TiposInstrucao::MULT) ? ciclosMult : ciclosDiv;
            instr.status.ciclosRestantesExecucao = lat;
            er.ciclosRestantes = lat;

            int rsIdx = regIndex(instr.regFonte1);
            if (rsIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte1);
            er.origemJ = estadoRegistradores[rsIdx].unidadeEscritora;
            if (er.origemJ.empty())
                er.valorJ = obterValorRegistrador(instr.regFonte1);

            int rtIdx = regIndex(instr.regFonte2);
            if (rtIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte2);
            er.origemK = estadoRegistradores[rtIdx].unidadeEscritora;
            if (er.origemK.empty())
                er.valorK = obterValorRegistrador(instr.regFonte2);

            int rdIdx = regIndex(instr.regDestino);
            if (rdIdx < 0) throw runtime_error("Registrador invalido: " + instr.regDestino);
            er.destReg = rdIdx;
            estadoRegistradores[rdIdx].unidadeEscritora = er.nome;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                          " emitida para " + er.nome + " (MUL/DIV).\n";
            return 0;
        } else if (instr.tipoInstrucao == TiposInstrucao::BNE) {
            int idx = encontrarERAddSubLivre();
            if (idx == -1) {
                logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                              " nao emitida (sem ER BNE).\n";
                return -1;
            }
            EstacaoReserva& er = estacoesAddSub[idx];
            er.ocupado = true;
            er.tipoInstrucao = TiposInstrucao::BNE;
            er.instrucao = &instr;
            instr.status.emitido = cicloAtual;
            instr.status.ciclosRestantesExecucao = 1;
            er.ciclosRestantes = 1;
            er.destReg = -1;

            int rsIdx = regIndex(instr.regFonte1);
            if (rsIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte1);
            er.origemJ = estadoRegistradores[rsIdx].unidadeEscritora;
            if (er.origemJ.empty())
                er.valorJ = obterValorRegistrador(instr.regFonte1);

            int rtIdx = regIndex(instr.regFonte2);
            if (rtIdx < 0) throw runtime_error("Registrador invalido: " + instr.regFonte2);
            er.origemK = estadoRegistradores[rtIdx].unidadeEscritora;
            if (er.origemK.empty())
                er.valorK = obterValorRegistrador(instr.regFonte2);

            branchPending = true;
            branchResolved = false;
            branchIssuedIndex = indiceInstrucao;

            logEventos += "-> Instrucao " + to_string(indiceInstrucao) +
                          " emitida para " + er.nome + " (BNE).\n";
            return 0;
        }

        return 0;
    }

    void executar() {
        for (int i = 0; i < numBuffersCarregamento; i++) {
            BufferLoad& lb = buffersCarregamento[i];
            if (!lb.ocupado || !lb.instrucao) continue;

            if (checarHazardLoadEForward(lb)) continue;

            if (lb.instrucao->status.inicioExecucao == -1) {
                if (lb.instrucao->status.emitido == cicloAtual) continue;
                lb.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> " + lb.nome + " iniciou execucao (LOAD).\n";
            }

            if (lb.ciclosRestantes > 0) {
                lb.ciclosRestantes--;
                lb.instrucao->status.ciclosRestantesExecucao = lb.ciclosRestantes;
                logEventos += "-> " + lb.nome + " completou 1 ciclo. Restantes: " +
                              to_string(lb.ciclosRestantes) + ".\n";
                if (lb.ciclosRestantes == 0) {
                    lb.instrucao->status.fimExecucao = cicloAtual;
                    lb.resultReady = true;
                    if (lb.hasForward)
                        lb.resultado = lb.forwardVal;
                    else {
                        int addr = lb.baseVal + lb.instrucao->offsetImediato;
                        lb.resultado = lerMemoria(addr);
                    }
                    logEventos += "-> " + lb.nome + " completou execucao.\n";
                }
            }
        }

        for (int i = 0; i < numBuffersArmazenamento; i++) {
            BufferStore& sb = buffersArmazenamento[i];
            if (!sb.ocupado || !sb.instrucao) continue;
            if (!sb.origemBase.empty()) continue;
            if (!sb.origemVal.empty()) continue;

            if (sb.instrucao->status.inicioExecucao == -1) {
                if (sb.instrucao->status.emitido == cicloAtual) continue;
                sb.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> " + sb.nome + " iniciou execucao (STORE).\n";
            }

            if (sb.ciclosRestantes > 0) {
                sb.ciclosRestantes--;
                sb.instrucao->status.ciclosRestantesExecucao = sb.ciclosRestantes;
                logEventos += "-> " + sb.nome + " completou 1 ciclo. Restantes: " +
                              to_string(sb.ciclosRestantes) + ".\n";
                if (sb.ciclosRestantes == 0) {
                    sb.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> " + sb.nome + " completou execucao.\n";
                }
            }
        }

        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            if (!er.ocupado || !er.instrucao) continue;
            if (!er.origemJ.empty() || !er.origemK.empty()) continue;

            if (er.instrucao->status.inicioExecucao == -1) {
                if (er.instrucao->status.emitido == cicloAtual) continue;
                er.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> " + er.nome + " iniciou execucao (" + er.tipoInstrucao + ").\n";
            }

            if (er.ciclosRestantes > 0) {
                er.ciclosRestantes--;
                er.instrucao->status.ciclosRestantesExecucao = er.ciclosRestantes;
                logEventos += "-> " + er.nome + " completou 1 ciclo. Restantes: " +
                              to_string(er.ciclosRestantes) + ".\n";
                if (er.ciclosRestantes == 0) {
                    er.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> " + er.nome + " completou execucao.\n";
                }
            }
        }

        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            if (!er.ocupado || !er.instrucao) continue;
            if (!er.origemJ.empty() || !er.origemK.empty()) continue;

            if (er.instrucao->status.inicioExecucao == -1) {
                if (er.instrucao->status.emitido == cicloAtual) continue;
                er.instrucao->status.inicioExecucao = cicloAtual;
                logEventos += "-> " + er.nome + " iniciou execucao (" + er.tipoInstrucao + ").\n";
            }

            if (er.ciclosRestantes > 0) {
                er.ciclosRestantes--;
                er.instrucao->status.ciclosRestantesExecucao = er.ciclosRestantes;
                logEventos += "-> " + er.nome + " completou 1 ciclo. Restantes: " +
                              to_string(er.ciclosRestantes) + ".\n";
                if (er.ciclosRestantes == 0) {
                    er.instrucao->status.fimExecucao = cicloAtual;
                    logEventos += "-> " + er.nome + " completou execucao.\n";
                }
            }
        }
    }

    void escreverResultado_CDB_unico() {
        bool escreveu = false;
        for (int turn = 0; turn < 3 && !escreveu; ++turn) {
            int cls = (cdb_rr + turn) % 3;

            if (cls == 0) {
                for (int i = 0; i < numEstacoesAddSub; i++) {
                    EstacaoReserva& er = estacoesAddSub[i];
                    if (!er.ocupado || !er.instrucao) continue;
                    if (er.ciclosRestantes != 0) continue;
                    if (er.instrucao->status.fimExecucao == cicloAtual) continue;
                    if (er.instrucao->status.escritaResultado != -1) continue;

                    if (er.tipoInstrucao == TiposInstrucao::BNE) {
                        int vj = er.valorJ;
                        int vk = er.valorK;
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

                        er.ocupado = false;
                        er.tipoInstrucao.clear();
                        er.origemJ.clear();
                        er.origemK.clear();
                        er.instrucao = nullptr;
                        er.destReg = -1;
                        er.ciclosRestantes = -1;

                        cdb_rr = (cls + 1) % 3;
                        escreveu = true;
                        break;
                    } else {
                        int resultado = 0;
                        if (er.tipoInstrucao == TiposInstrucao::SOMA)
                            resultado = er.valorJ + er.valorK;
                        else if (er.tipoInstrucao == TiposInstrucao::SUBT)
                            resultado = er.valorJ - er.valorK;

                        er.instrucao->status.escritaResultado = cicloAtual;
                        logEventos += "-> " + er.nome + " escreveu resultado no CDB.\n";

                        if (er.destReg >= 0 &&
                            estadoRegistradores[er.destReg].unidadeEscritora == er.nome) {
                            estadoRegistradores[er.destReg].unidadeEscritora.clear();
                            registradores[er.destReg].valor = resultado;
                        }

                        transmitirResultado(resultado, er.nome);

                        er.ocupado = false;
                        er.tipoInstrucao.clear();
                        er.instrucao = nullptr;
                        er.origemJ.clear();
                        er.origemK.clear();
                        er.ciclosRestantes = -1;
                        er.destReg = -1;

                        cdb_rr = (cls + 1) % 3;
                        escreveu = true;
                        break;
                    }
                }
            } else if (cls == 1) {
                for (int i = 0; i < numEstacoesMultDiv; i++) {
                    EstacaoReserva& er = estacoesMultDiv[i];
                    if (!er.ocupado || !er.instrucao) continue;
                    if (er.ciclosRestantes != 0) continue;
                    if (er.instrucao->status.fimExecucao == cicloAtual) continue;
                    if (er.instrucao->status.escritaResultado != -1) continue;

                    int resultado = 0;
                    if (er.tipoInstrucao == TiposInstrucao::MULT)
                        resultado = er.valorJ * er.valorK;
                    else if (er.tipoInstrucao == TiposInstrucao::DIVI)
                        resultado = (er.valorK == 0) ? 0 : er.valorJ / er.valorK;

                    er.instrucao->status.escritaResultado = cicloAtual;
                    logEventos += "-> " + er.nome + " escreveu resultado no CDB.\n";

                    if (er.destReg >= 0 &&
                        estadoRegistradores[er.destReg].unidadeEscritora == er.nome) {
                        estadoRegistradores[er.destReg].unidadeEscritora.clear();
                        registradores[er.destReg].valor = resultado;
                    }

                    transmitirResultado(resultado, er.nome);

                    er.ocupado = false;
                    er.tipoInstrucao.clear();
                    er.instrucao = nullptr;
                    er.origemJ.clear();
                    er.origemK.clear();
                    er.ciclosRestantes = -1;
                    er.destReg = -1;

                    cdb_rr = (cls + 1) % 3;
                    escreveu = true;
                    break;
                }
            } else {
                for (int i = 0; i < numBuffersCarregamento; i++) {
                    BufferLoad& lb = buffersCarregamento[i];
                    if (!lb.ocupado || !lb.instrucao) continue;
                    if (!lb.resultReady) continue;
                    if (lb.instrucao->status.escritaResultado != -1) continue;

                    lb.instrucao->status.escritaResultado = cicloAtual;
                    logEventos += "-> " + lb.nome + " escreveu resultado no CDB.\n";

                    if (lb.destReg >= 0 &&
                        estadoRegistradores[lb.destReg].unidadeEscritora == lb.nome) {
                        estadoRegistradores[lb.destReg].unidadeEscritora.clear();
                        registradores[lb.destReg].valor = lb.resultado;
                    }

                    transmitirResultado(lb.resultado, lb.nome);

                    lb.ocupado = false;
                    lb.instrucao = nullptr;
                    lb.ciclosRestantes = -1;
                    lb.resultReady = false;
                    lb.origemBase.clear();
                    lb.hasForward = false;
                    lb.forwardVal = 0;

                    cdb_rr = (cls + 1) % 3;
                    escreveu = true;
                    break;
                }
            }
        }
    }

    void escreverResultado_STOREs() {
        struct Cand { BufferStore* buf; int emit; };
        vector<Cand> prontos;

        for (int i = 0; i < numBuffersArmazenamento; ++i) {
            BufferStore& sb = buffersArmazenamento[i];
            if (!sb.ocupado || !sb.instrucao) continue;
            if (sb.ciclosRestantes != 0) continue;
            if (sb.instrucao->status.fimExecucao == -1) continue;
            if (sb.instrucao->status.escritaResultado != -1) continue;
            prontos.push_back({ &sb, sb.instrucao->status.emitido });
        }

        if (prontos.empty()) return;

        sort(prontos.begin(), prontos.end(),
             [](const Cand& a, const Cand& b){ return a.emit < b.emit; });

        for (auto& c : prontos) {
            BufferStore& sb = *c.buf;
            if (!sb.origemBase.empty() || !sb.origemVal.empty()) continue;

            int addr = sb.baseVal + sb.instrucao->offsetImediato;
            int val  = sb.value;
            escreverMemoria(addr, val);

            sb.instrucao->status.escritaResultado = cicloAtual;
            logEventos += "-> " + sb.nome + " comitou na memoria ["
                          + to_string(addr) + "]=" + to_string(val) + ".\n";

            sb.ocupado = false;
            sb.instrucao = nullptr;
            sb.ciclosRestantes = -1;
            sb.origemBase.clear();
            sb.origemVal.clear();
        }
    }

    bool haTrabalhoPendente(int proxIndiceInstrucao) const {
        if (proxIndiceInstrucao < numInstrucoes && !branchPending) return true;
        for (int i = 0; i < numBuffersCarregamento; ++i)
            if (buffersCarregamento[i].ocupado) return true;
        for (int i = 0; i < numBuffersArmazenamento; ++i)
            if (buffersArmazenamento[i].ocupado) return true;
        for (int i = 0; i < numEstacoesAddSub; ++i)
            if (estacoesAddSub[i].ocupado) return true;
        for (int i = 0; i < numEstacoesMultDiv; ++i)
            if (estacoesMultDiv[i].ocupado) return true;
        for (int i = 0; i < numInstrucoes; ++i)
            if (instrucoes[i].status.emitido != -1 &&
                instrucoes[i].status.escritaResultado == -1)
                return true;
        return false;
    }

    void mostrarEstado() {
        int y = 2;
        irPara(2, y); cout << "Instrucoes:";
        irPara(27, y); cout << "Emitido" << " Comeco" << " Fim" << " Escrita";
        irPara(27, y + 1); cout << "__________________________________";

        int offset = 0;
        for (int i = 0; i < numInstrucoes; i++) {
            irPara(2, offset + y + 2);
            string instrStr = to_string(i) + ". " + instrucoes[i].tipoInstrucao + " ";
            if (instrucoes[i].tipoInstrucao == TiposInstrucao::CARREGA ||
                instrucoes[i].tipoInstrucao == TiposInstrucao::ARMAZENA) {
                instrStr += instrucoes[i].regFonte2 + ", " +
                            to_string(instrucoes[i].offsetImediato) + "(" +
                            instrucoes[i].regFonte1 + ")";
            } else if (instrucoes[i].tipoInstrucao == TiposInstrucao::BNE) {
                instrStr += instrucoes[i].regFonte1 + ", " +
                            instrucoes[i].regFonte2 + ", " +
                            to_string(instrucoes[i].offsetImediato);
            } else {
                instrStr += instrucoes[i].regDestino + ", " +
                            instrucoes[i].regFonte1 + ", " +
                            instrucoes[i].regFonte2;
            }
            cout << left << setw(24) << instrStr;

            irPara(27, offset + y + 2);
            cout << "|" << right << setw(7)
                 << (instrucoes[i].status.emitido == -1 ? "" : to_string(instrucoes[i].status.emitido))
                 << "|" << setw(7)
                 << (instrucoes[i].status.inicioExecucao == -1 ? "" : to_string(instrucoes[i].status.inicioExecucao))
                 << "|" << setw(7)
                 << (instrucoes[i].status.fimExecucao == -1 ? "" : to_string(instrucoes[i].status.fimExecucao))
                 << "|" << setw(9)
                 << (instrucoes[i].status.escritaResultado == -1 ? "" : to_string(instrucoes[i].status.escritaResultado))
                 << "|";

            offset++;
            irPara(27, offset + y + 2);
            cout << "|_______|_______|_______|_________|";
            offset++;
        }

        int yLS = 2;
        irPara(70, yLS);
        cout << "Load/Store Buffers: Ocupado Endereco Qbase Vbase Qval Vval Rest.";
        yLS++;
        irPara(72, yLS); cout << "__________________________________________________________";

        for (int i = 0; i < numBuffersCarregamento; i++) {
            yLS++;
            irPara(70, yLS);
            BufferLoad& lb = buffersCarregamento[i];
            cout << right << setw(8) << lb.nome;
            cout << " |" << setw(7) << (lb.ocupado ? "Sim" : "Nao");
            string endStr = lb.ocupado && lb.origemBase.empty()
                ? to_string(lb.baseVal) + "+" + to_string(lb.instrucao->offsetImediato)
                : "";
            cout << "|" << setw(9) << endStr;
            cout << "|" << setw(6) << lb.origemBase;
            cout << "|" << setw(6) << (lb.origemBase.empty() && lb.ocupado ? to_string(lb.baseVal) : "");
            cout << "|" << setw(6) << "";
            cout << "|" << setw(6) << (lb.resultReady ? to_string(lb.resultado) : "");
            cout << "|" << setw(5) << (lb.ocupado ? to_string(max(lb.ciclosRestantes,0)) : "") << "|";
            yLS++;
            irPara(78, yLS); cout << "|_______|_________|______|______|______|______|_____|";
        }
        for (int i = 0; i < numBuffersArmazenamento; i++) {
            yLS++;
            irPara(70, yLS);
            BufferStore& sb = buffersArmazenamento[i];
            cout << right << setw(8) << sb.nome;
            cout << " |" << setw(7) << (sb.ocupado ? "Sim" : "Nao");
            string endStr = sb.ocupado && sb.origemBase.empty()
                ? to_string(sb.baseVal) + "+" + to_string(sb.instrucao->offsetImediato)
                : "";
            cout << "|" << setw(9) << endStr;
            cout << "|" << setw(6) << sb.origemBase;
            cout << "|" << setw(6) << (sb.origemBase.empty() && sb.ocupado ? to_string(sb.baseVal) : "");
            cout << "|" << setw(6) << sb.origemVal;
            cout << "|" << setw(6) << (sb.origemVal.empty() && sb.ocupado ? to_string(sb.value) : "");
            cout << "|" << setw(5) << (sb.ocupado ? to_string(max(sb.ciclosRestantes,0)) : "") << "|";
            yLS++;
            irPara(78, yLS); cout << "|_______|_________|______|______|______|______|_____|";
        }

        int yRegs = (offset + y + 2 > yLS ? offset + y + 2 : yLS) + 3;
        irPara(90, yRegs); cout << "Registradores (Valores):";
        irPara(90, ++yRegs); cout << " Nome  Valor";
        irPara(90, ++yRegs); cout << "____________";

        for (size_t i = 0; i < registradores.size(); ++i) {
            yRegs++;
            irPara(90, yRegs);
            cout << "| " << left << setw(4) << registradores[i].nome
                 << "| " << right << setw(5) << registradores[i].valor << "|";
        }
        if (!registradores.empty()) {
            yRegs++;
            irPara(90, yRegs); cout << "|_____|_______|";
        }

        yRegs++;
        irPara(90, ++yRegs); cout << "Memoria";
        irPara(90, ++yRegs); cout << " End.  Valor";
        irPara(90, ++yRegs); cout << "____________";
        for (const auto& m : memoria) {
            yRegs++;
            irPara(90, yRegs);
            cout << "| " << left << setw(4) << m.nome
                 << "| " << right << setw(5) << m.valor << "|";
        }
        if (!memoria.empty()) {
            yRegs++;
            irPara(90, yRegs); cout << "|____|_______|";
        }

        int yER = (yRegs > yLS ? yRegs : yLS) + 3;
        irPara(4, yER); cout << "Estacoes de Reserva (ERs):";
        yER++;
        irPara(21, yER); cout << " Nome  Ocup  Op  Vj   Vk   Qj      Qk      Rest.";
        yER++;
        irPara(28, yER); cout << "_________________________________________________";

        for (int i = 0; i < numEstacoesAddSub; i++) {
            EstacaoReserva& er = estacoesAddSub[i];
            yER++;
            irPara(19, yER);
            cout << right << setw(6) << er.nome
                 << " |" << setw(4) << (er.ocupado ? "Sim" : "Nao")
                 << "|" << setw(4) << er.tipoInstrucao
                 << "|" << setw(4) << (er.origemJ.empty() && er.ocupado ? to_string(er.valorJ) : "")
                 << "|" << setw(4) << (er.origemK.empty() && er.ocupado ? to_string(er.valorK) : "")
                 << "|" << setw(7) << er.origemJ
                 << "|" << setw(7) << er.origemK
                 << "|" << setw(5) << (er.ocupado ? to_string(max(er.ciclosRestantes,0)) : "") << "|";
            yER++;
            irPara(25, yER); cout << "|____|____|____|____|_______|_______|_____|";
        }
        for (int i = 0; i < numEstacoesMultDiv; i++) {
            EstacaoReserva& er = estacoesMultDiv[i];
            yER++;
            irPara(19, yER);
            cout << right << setw(6) << er.nome
                 << " |" << setw(4) << (er.ocupado ? "Sim" : "Nao")
                 << "|" << setw(4) << er.tipoInstrucao
                 << "|" << setw(4) << (er.origemJ.empty() && er.ocupado ? to_string(er.valorJ) : "")
                 << "|" << setw(4) << (er.origemK.empty() && er.ocupado ? to_string(er.valorK) : "")
                 << "|" << setw(7) << er.origemJ
                 << "|" << setw(7) << er.origemK
                 << "|" << setw(5) << (er.ocupado ? to_string(max(er.ciclosRestantes,0)) : "") << "|";
            yER++;
            irPara(25, yER); cout << "|____|____|____|____|_______|_______|_____|";
        }

        int yStatusReg = yER + 3;
        irPara(20, yStatusReg); cout << "Estado dos Registradores (Unidade Escritora - Q.i):";
        yStatusReg++;

        int xPos = 20;
        for (int i = 0; i < numTotalRegistradores; i++) {
            irPara(xPos, yStatusReg);
            cout << right << setw(5) << estadoRegistradores[i].nomeRegistrador;
            irPara(xPos, yStatusReg + 1); cout << "______";
            irPara(xPos, yStatusReg + 2);
            cout << "|" << setw(4) << estadoRegistradores[i].unidadeEscritora << "|";
            irPara(xPos, yStatusReg + 3); cout << "|______|";
            xPos += 8;
        }

        irPara(2, yStatusReg + 5);
        cout << "\n\nEventos do Ciclo " << cicloAtual - 1 << " (Log): \n"
             << logEventos;
    }

    void Simular() {
        int proxIndiceInstrucao = 0;
        cicloAtual = 1;

        while (true) {
            irPara(0, 0);
            cout << "Ciclo Atual: " << cicloAtual;
            mostrarEstado();

            cout << "\n\n\n\nPressione ENTER para o proximo ciclo: ";
            cin.sync();
            cin.get();

            logEventos.clear();

            escreverResultado_CDB_unico();
            escreverResultado_STOREs();
            executar();

            if (branchResolved) {
                if (branchTaken)
                    proxIndiceInstrucao = branchTarget;
                else
                    proxIndiceInstrucao = branchIssuedIndex + 1;

                branchPending = false;
                branchResolved = false;
                branchTaken = false;
                branchTarget = -1;
                branchIssuedIndex = -1;
            }

            int resIssue = -1;
            if (!branchPending && proxIndiceInstrucao < numInstrucoes)
                resIssue = emitirInstrucao(proxIndiceInstrucao);
            if (resIssue != -1 && resIssue != -2) proxIndiceInstrucao++;

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

    Tomasulo() {}
    ~Tomasulo() {
        delete[] buffersCarregamento;
        delete[] buffersArmazenamento;
        delete[] estacoesAddSub;
        delete[] estacoesMultDiv;
        delete[] estadoRegistradores;
        delete[] instrucoes;
    }
};

int main() {
#if defined(_WIN32)
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(
        hConsole,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
    );
    system("cls");
#endif

    Tomasulo simulador;
    simulador.carregarDadosDoArquivo("source.txt");
    simulador.Simular();
    return 0;
}

// posiciona cursor no console
void irPara(short x, short y) {
    COORD c = { x, y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), c);
}
