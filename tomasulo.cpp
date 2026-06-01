#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static vector<string> split(const string& s) {
    vector<string> v;
    string x;
    istringstream iss(s);
    while (iss >> x) v.push_back(x);
    return v;
}

static string upperStr(string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static bool isIntegerLine(const string& s) {
    string t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    if (t[i] == '+' || t[i] == '-') i++;
    if (i >= t.size()) return false;
    for (; i < t.size(); i++) if (!isdigit((unsigned char)t[i])) return false;
    return true;
}

static int toInt(const string& s, const string& what = "valor inteiro") {
    try {
        size_t p = 0;
        int v = stoi(s, &p);
        if (p != s.size()) throw invalid_argument("resto");
        return v;
    } catch (...) {
        throw runtime_error("Esperado " + what + ", recebido: '" + s + "'");
    }
}

class Tomasulo {
public:
    enum class Op { ADD, SUB, MUL, DIV, LOAD, STORE, BNE, BEQ, INVALID };

    struct Config {
        int addStations = 3;
        int mulStations = 2;
        int loadBuffers = 2;
        int storeBuffers = 2;

        int addCycles = 2;
        int mulCycles = 4;
        int loadStoreCycles = 2;
        int divCycles = 8;
        int branchCycles = 1;

        int numRegs = 10;
        int issueWidth = 2;
        int cdbWidth = 2;
        int commitWidth = 2;
        int robEntries = 32;

        int addFunctionalUnits = 2;
        int mulFunctionalUnits = 1;
        int loadStoreFunctionalUnits = 1;
        int branchFunctionalUnits = 1;

        string branchMode = "Stall"; 
    } cfg;

    struct Instrucao {
        Op op = Op::INVALID;
        string opText;
        int rd = -1;
        int rs = -1;
        int rt = -1;
        int offset = 0;
        string original;
    };

    struct Reg {
        int value = 0;
    };

    struct DynInstr {
        int id = -1;
        int pc = -1;
        Instrucao inst;
        string robTag;
        string unitName;
        bool squashed = false;
        int issue = -1;
        int start = -1;
        int end = -1;
        int wb = -1;
        int commit = -1;
    };

    struct ROBEntry {
        int id = -1;
        int pc = -1;
        Op op = Op::INVALID;
        string tag;
        string instrText;
        int destReg = -1;
        int value = 0;
        bool ready = false;
        bool squashed = false;

        int storeAddr = 0;
        int storeValue = 0;
        bool addressReady = false;
        bool storeValueReady = false;

        bool branchTaken = false;
        int branchTarget = -1;
        int predictedNextPC = -1;
        vector<string> ratSnapshot;
    };

    struct RSEntry {
        string name;
        bool busy = false;
        Op op = Op::INVALID;
        int vj = 0, vk = 0;
        string qj, qk;
        string destTag;
        int dynId = -1;
        bool executing = false;
        bool waitingWB = false;
        int result = 0;
        bool branchTaken = false;
        int branchTarget = -1;
        int readyCycle = -1; 
    };

    struct LSEntry {
        string name;
        bool busy = false;
        bool isLoad = true;
        int baseV = 0;
        string baseQ;
        int valueV = 0;
        string valueQ;
        int offset = 0;
        int address = 0;
        bool addressKnown = false;
        string destTag;
        int dynId = -1;
        bool executing = false;
        bool waitingWB = false;
        bool completedStore = false;
        int result = 0;
        int readyCycle = -1; 
    };

    struct FU {
        string name;
        string kind;       
        bool busy = false;
        bool rs = true;    
        int index = -1;
        int dynId = -1;
        int remaining = 0;
    };

    struct PendingWB {
        int readyCycle = -1;
        int dynId = -1;
        string tag;
        Op op = Op::INVALID;
        int result = 0;
        bool branchTaken = false;
        int branchTarget = -1;
    };

private:
    vector<Instrucao> programa;
    vector<Reg> regs;
    vector<string> rat;
    map<int, int> memoria;
    set<int> regsUsados;

    vector<RSEntry> rsAdd;
    vector<RSEntry> rsMul;
    vector<LSEntry> loadBuf;
    vector<LSEntry> storeBuf;

    vector<FU> fuAdd;
    vector<FU> fuMul;
    vector<FU> fuLS;
    vector<FU> fuBR;

    deque<ROBEntry> rob;
    vector<DynInstr> historico;
    vector<PendingWB> pendingWB;

    int pc = 0;
    int ciclo = 0;
    int nextDynId = 1;
    bool branchBlocked = false;
    string eventos;

    long long issueSlotsVazios = 0;
    long long ciclosSemIssue = 0;
    long long totalIssued = 0;
    long long totalCommitted = 0;
    long long totalSquashed = 0;

public:
    Tomasulo() = default;

    void carregarArquivo(const string& filename) {
        ifstream in(filename);
        if (!in) throw runtime_error("Nao foi possivel abrir '" + filename + "'");

        vector<string> lines;
        string line;
        while (getline(in, line)) {
            size_t p = line.find('#');
            if (p != string::npos) line = line.substr(0, p);
            line = trim(line);
            if (!line.empty()) lines.push_back(line);
        }
        if (lines.empty()) throw runtime_error("Arquivo vazio");

        size_t i = 0;
        bool gotRegisters = false;
        while (i < lines.size()) {
            vector<string> t = split(lines[i]);
            if (t.empty()) { i++; continue; }
            string key = upperStr(t[0]);

            if (key == "REGISTERS") {
                if (t.size() != 2) throw runtime_error("Linha Registers deve ser: Registers <n>");
                cfg.numRegs = toInt(t[1], "quantidade de registradores");
                gotRegisters = true;
                i++;
                break;
            }

            parseConfigLine(t);
            i++;
        }
        if (!gotRegisters) throw runtime_error("Faltou linha: Registers <n>");
        if (cfg.numRegs <= 0) throw runtime_error("Registers precisa ser maior que zero");

        regs.assign(cfg.numRegs, Reg{});
        rat.assign(cfg.numRegs, "");

        while (i < lines.size()) {
            if (isIntegerLine(lines[i])) break;
            vector<string> t = split(lines[i]);
            if (t.empty()) { i++; continue; }
            string key = upperStr(t[0]);

            if (key == "MEMORY") {
                if (t.size() != 3) throw runtime_error("Memory deve ser: Memory <endereco> <valor>");
                
                memoria[toInt(t[1], "endereco de memoria") & ~3] = toInt(t[2], "valor de memoria");
            } else if (isMemoryToken(key)) {
                if (t.size() != 2) throw runtime_error("Inicializacao de memoria deve ser: M<endereco> <valor>");
                
                int addr = parseMemoryToken(key) & ~3;
                memoria[addr] = toInt(t[1], "valor de memoria");
            } else {
                if (t.size() != 2) throw runtime_error("Inicializacao invalida: " + lines[i]);
                int r = regIndex(t[0]);
                if (r < 0 || r >= cfg.numRegs) throw runtime_error("Registrador invalido na inicializacao: " + t[0]);
                regs[r].value = toInt(t[1], "valor de registrador");
            }
            i++;
        }

        if (i >= lines.size() || !isIntegerLine(lines[i])) throw runtime_error("Faltou numero de instrucoes");
        int n = toInt(lines[i], "numero de instrucoes");
        i++;
        if (n < 0) throw runtime_error("Numero de instrucoes invalido");
        if (i + (size_t)n > lines.size()) throw runtime_error("Menos instrucoes do que o numero informado");

        for (int k = 0; k < n; k++, i++) {
            programa.push_back(parseInstruction(lines[i]));
        }

        inicializarEstruturas();
    }

    void simular(bool passoAPasso, int maxCiclos) {
        if (programa.empty()) throw runtime_error("Programa sem instrucoes");
        ciclo = 0;

        while (true) {
            ciclo++;
            eventos.clear();

            escreverCDB();
            executar();
            commitROB();
            emitirSuperescalar();

            limparTela();
            mostrarEstado();

            if (concluido()) {
                cout << "\nSimulacao concluida no ciclo " << ciclo << ".\n";
                mostrarResumoFinal();
                break;
            }

            if (maxCiclos > 0 && ciclo >= maxCiclos) {
                cout << "\nSimulacao interrompida por limite de ciclos (" << maxCiclos << ").\n";
                mostrarResumoFinal();
                break;
            }

            if (passoAPasso) {
                cout << "\nPressione ENTER para o proximo ciclo...";
                cin.get();
            }
        }
    }

private:
    static bool isMemoryToken(const string& s) {
        if (s.size() < 2 || s[0] != 'M') return false;
        for (size_t i = 1; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) return false;
        return true;
    }

    static int parseMemoryToken(const string& s) {
        return toInt(s.substr(1), "endereco de memoria");
    }

    void parseConfigLine(const vector<string>& t) {
        if (t.empty()) return;
        string key = upperStr(t[0]);
        auto need2 = [&]() {
            if (t.size() != 2) throw runtime_error("Configuracao invalida: " + t[0]);
            return t[1];
        };
        auto val = [&]() { return max(1, toInt(need2(), "valor de configuracao")); };

        if (key == "ADD_SUB_RESERVATION_STATIONS") cfg.addStations = val();
        else if (key == "MUL_DIV_RESERVATION_STATIONS") cfg.mulStations = val();
        else if (key == "LOAD_BUFFERS") cfg.loadBuffers = val();
        else if (key == "STORE_BUFFERS") cfg.storeBuffers = val();
        else if (key == "ADD_SUB_CYCLES") cfg.addCycles = val();
        else if (key == "MUL_CYCLES") cfg.mulCycles = val();
        else if (key == "LOAD_STORE_CYCLES") cfg.loadStoreCycles = val();
        else if (key == "DIV_CYCLES") cfg.divCycles = val();
        else if (key == "BRANCH_CYCLES") cfg.branchCycles = val();
        else if (key == "ISSUE_WIDTH") cfg.issueWidth = val();
        else if (key == "CDB_WIDTH" || key == "WRITE_RESULT_WIDTH") cfg.cdbWidth = val();
        else if (key == "COMMIT_WIDTH") cfg.commitWidth = val();
        else if (key == "ROB_ENTRIES") cfg.robEntries = val();
        else if (key == "ADD_SUB_FUNCTIONAL_UNITS") cfg.addFunctionalUnits = val();
        else if (key == "MUL_DIV_FUNCTIONAL_UNITS") cfg.mulFunctionalUnits = val();
        else if (key == "LOAD_STORE_FUNCTIONAL_UNITS") cfg.loadStoreFunctionalUnits = val();
        else if (key == "BRANCH_FUNCTIONAL_UNITS") cfg.branchFunctionalUnits = val();
        else if (key == "BRANCH_MODE") {
            string m = need2();
            string u = upperStr(m);
            if (u == "STALL") cfg.branchMode = "Stall";
            else if (u == "PREDICT_NOT_TAKEN" || u == "PREDICTNOTTAKEN") cfg.branchMode = "Predict_Not_Taken";
            else throw runtime_error("Branch_Mode invalido. Use Stall ou Predict_Not_Taken");
        } else {
            throw runtime_error("Configuracao desconhecida: " + t[0]);
        }
    }

    int regIndex(const string& raw) const {
        string r = raw;
        if (!r.empty() && r[0] == '$') r = r.substr(1);
        if (r.size() >= 2 && (r[0] == 'F' || r[0] == 'f' || r[0] == 'R' || r[0] == 'r')) {
            for (size_t i = 1; i < r.size(); i++) if (!isdigit((unsigned char)r[i])) return -1;
            return toInt(r.substr(1), "indice de registrador");
        }
        return -1;
    }

    string regName(int r) const {
        if (r < 0) return "-";
        return "F" + to_string(r);
    }

    Op parseOp(const string& s) const {
        string op = upperStr(s);
        if (op == "ADD") return Op::ADD;
        if (op == "SUB") return Op::SUB;
        if (op == "MUL") return Op::MUL;
        if (op == "DIV") return Op::DIV;
        if (op == "LOAD" || op == "LW") return Op::LOAD;
        if (op == "STORE" || op == "SW") return Op::STORE;
        if (op == "BNE") return Op::BNE;
        if (op == "BEQ") return Op::BEQ;
        return Op::INVALID;
    }

    string opName(Op op) const {
        switch (op) {
            case Op::ADD: return "ADD";
            case Op::SUB: return "SUB";
            case Op::MUL: return "MUL";
            case Op::DIV: return "DIV";
            case Op::LOAD: return "LOAD";
            case Op::STORE: return "STORE";
            case Op::BNE: return "BNE";
            case Op::BEQ: return "BEQ";
            default: return "?";
        }
    }

    bool writesRegister(Op op) const {
        return op == Op::ADD || op == Op::SUB || op == Op::MUL || op == Op::DIV || op == Op::LOAD;
    }

    Instrucao parseInstruction(const string& line) {
        vector<string> t = split(line);
        if (t.empty()) throw runtime_error("Instrucao vazia");
        Instrucao inst;
        inst.original = line;
        inst.op = parseOp(t[0]);
        inst.opText = opName(inst.op);
        if (inst.op == Op::INVALID) throw runtime_error("Operacao desconhecida: " + t[0]);

        auto parseRegChecked = [&](const string& s) {
            int r = regIndex(s);
            if (r < 0) throw runtime_error("Registrador invalido na instrucao '" + line + "': " + s);
            return r;
        };

        if (inst.op == Op::ADD || inst.op == Op::SUB || inst.op == Op::MUL || inst.op == Op::DIV) {
            if (t.size() != 4) throw runtime_error("Formato esperado: OP destino fonte1 fonte2. Linha: " + line);
            inst.rd = parseRegChecked(t[1]);
            inst.rs = parseRegChecked(t[2]);
            inst.rt = parseRegChecked(t[3]);
        } else if (inst.op == Op::LOAD) {
            if (t.size() != 4) throw runtime_error("Formato esperado: LOAD destino offset base. Linha: " + line);
            inst.rd = parseRegChecked(t[1]);
            inst.offset = toInt(t[2], "offset");
            inst.rs = parseRegChecked(t[3]);
        } else if (inst.op == Op::STORE) {
            if (t.size() != 4) throw runtime_error("Formato esperado: STORE valor offset base. Linha: " + line);
            inst.rt = parseRegChecked(t[1]);
            inst.offset = toInt(t[2], "offset");
            inst.rs = parseRegChecked(t[3]);
        } else if (inst.op == Op::BNE || inst.op == Op::BEQ) {
            if (t.size() != 4) throw runtime_error("Formato esperado: BNE/BEQ fonte1 fonte2 offset. Linha: " + line);
            inst.rs = parseRegChecked(t[1]);
            inst.rt = parseRegChecked(t[2]);
            inst.offset = toInt(t[3], "offset de branch");
        }

        if (inst.rd >= cfg.numRegs || inst.rs >= cfg.numRegs || inst.rt >= cfg.numRegs) {
            throw runtime_error("Instrucao usa registrador fora do limite de Registers: " + line);
        }
        if (inst.rd >= 0) regsUsados.insert(inst.rd);
        if (inst.rs >= 0) regsUsados.insert(inst.rs);
        if (inst.rt >= 0) regsUsados.insert(inst.rt);
        return inst;
    }

    void inicializarEstruturas() {
        rsAdd.assign(cfg.addStations, RSEntry{});
        rsMul.assign(cfg.mulStations, RSEntry{});
        loadBuf.assign(cfg.loadBuffers, LSEntry{});
        storeBuf.assign(cfg.storeBuffers, LSEntry{});

        for (int i = 0; i < cfg.addStations; i++) rsAdd[i].name = "Add" + to_string(i + 1);
        for (int i = 0; i < cfg.mulStations; i++) rsMul[i].name = "Mult" + to_string(i + 1);
        for (int i = 0; i < cfg.loadBuffers; i++) { loadBuf[i].name = "Load" + to_string(i + 1); loadBuf[i].isLoad = true; }
        for (int i = 0; i < cfg.storeBuffers; i++) { storeBuf[i].name = "Store" + to_string(i + 1); storeBuf[i].isLoad = false; }

        for (int i = 0; i < cfg.addFunctionalUnits; i++) fuAdd.push_back(FU{"AddFU" + to_string(i + 1), "ADD", false, true, -1, -1, 0});
        for (int i = 0; i < cfg.mulFunctionalUnits; i++) fuMul.push_back(FU{"MulFU" + to_string(i + 1), "MUL", false, true, -1, -1, 0});
        for (int i = 0; i < cfg.loadStoreFunctionalUnits; i++) fuLS.push_back(FU{"LSFU" + to_string(i + 1), "LS", false, false, -1, -1, 0});
        for (int i = 0; i < cfg.branchFunctionalUnits; i++) fuBR.push_back(FU{"BrFU" + to_string(i + 1), "BR", false, true, -1, -1, 0});
    }

    DynInstr& dyn(int dynId) {
        if (dynId <= 0 || dynId > (int)historico.size()) throw runtime_error("dynId invalido");
        return historico[dynId - 1];
    }

    const DynInstr& dyn(int dynId) const {
        if (dynId <= 0 || dynId > (int)historico.size()) throw runtime_error("dynId invalido");
        return historico[dynId - 1];
    }

    ROBEntry* robByTag(const string& tag) {
        for (auto& e : rob) if (e.tag == tag) return &e;
        return nullptr;
    }

    ROBEntry* robByDyn(int dynId) {
        for (auto& e : rob) if (e.id == dynId) return &e;
        return nullptr;
    }

    struct Operand {
        int value = 0;
        string q;
    };

    Operand readOperand(int reg) {
        if (reg < 0) return {};
        if (!rat[reg].empty()) {
            ROBEntry* e = robByTag(rat[reg]);
            if (e && e->ready && writesRegister(e->op)) return {e->value, ""};
            if (e) return {0, rat[reg]};
            rat[reg].clear(); 
        }
        return {regs[reg].value, ""};
    }

    int latency(Op op) const {
        switch (op) {
            case Op::ADD:
            case Op::SUB: return cfg.addCycles;
            case Op::MUL: return cfg.mulCycles;
            case Op::DIV: return cfg.divCycles;
            case Op::LOAD:
            case Op::STORE: return cfg.loadStoreCycles;
            case Op::BNE:
            case Op::BEQ: return cfg.branchCycles;
            default: return 1;
        }
    }

    int findFreeAddRS() const { for (int i = 0; i < (int)rsAdd.size(); i++) if (!rsAdd[i].busy) return i; return -1; }
    int findFreeMulRS() const { for (int i = 0; i < (int)rsMul.size(); i++) if (!rsMul[i].busy) return i; return -1; }
    int findFreeLoad() const { for (int i = 0; i < (int)loadBuf.size(); i++) if (!loadBuf[i].busy) return i; return -1; }
    int findFreeStore() const { for (int i = 0; i < (int)storeBuf.size(); i++) if (!storeBuf[i].busy) return i; return -1; }

    bool robFull() const { return (int)rob.size() >= cfg.robEntries; }

    string instrToString(const Instrucao& in) const {
        ostringstream os;
        os << opName(in.op) << " ";
        if (in.op == Op::LOAD) os << regName(in.rd) << "," << in.offset << "(" << regName(in.rs) << ")";
        else if (in.op == Op::STORE) os << regName(in.rt) << "," << in.offset << "(" << regName(in.rs) << ")";
        else if (in.op == Op::BNE || in.op == Op::BEQ) os << regName(in.rs) << "," << regName(in.rt) << "," << in.offset;
        else os << regName(in.rd) << "," << regName(in.rs) << "," << regName(in.rt);
        return os.str();
    }

    void emitirSuperescalar() {
        int emitidas = 0;
        string motivoParada;

        while (emitidas < cfg.issueWidth) {
            if (pc < 0 || pc >= (int)programa.size()) break;
            if (cfg.branchMode == "Stall" && branchBlocked) { motivoParada = "branch pendente"; break; }
            if (robFull()) { motivoParada = "ROB cheio"; break; }

            const Instrucao& in = programa[pc];
            int slot = -1;
            if (in.op == Op::ADD || in.op == Op::SUB) slot = findFreeAddRS();
            else if (in.op == Op::MUL || in.op == Op::DIV) slot = findFreeMulRS();
            else if (in.op == Op::LOAD) slot = findFreeLoad();
            else if (in.op == Op::STORE) slot = findFreeStore();
            else if (in.op == Op::BNE || in.op == Op::BEQ) slot = findFreeAddRS();

            if (slot < 0) {
                motivoParada = "sem estacao/buffer livre para " + opName(in.op);
                break;
            }

            int dynId = nextDynId++;
            string tag = "ROB" + to_string(dynId);

            DynInstr d;
            d.id = dynId;
            d.pc = pc;
            d.inst = in;
            d.robTag = tag;
            d.issue = ciclo;
            historico.push_back(d);

            ROBEntry re;
            re.id = dynId;
            re.pc = pc;
            re.op = in.op;
            re.tag = tag;
            re.instrText = instrToString(in);
            re.destReg = writesRegister(in.op) ? in.rd : -1;
            if (in.op == Op::BNE || in.op == Op::BEQ) {
                re.predictedNextPC = pc + 1;
                re.ratSnapshot = rat;
            }
            rob.push_back(re);

            if (in.op == Op::ADD || in.op == Op::SUB || in.op == Op::BNE || in.op == Op::BEQ) {
                RSEntry& rs = rsAdd[slot];
                rs = RSEntry{};
                rs.name = "Add" + to_string(slot + 1);
                rs.busy = true;
                rs.op = in.op;
                rs.destTag = tag;
                rs.dynId = dynId;
                Operand a = readOperand(in.rs);
                Operand b = readOperand(in.rt);
                rs.vj = a.value; rs.qj = a.q;
                rs.vk = b.value; rs.qk = b.q;
                rs.readyCycle = (rs.qj.empty() && rs.qk.empty()) ? ciclo : -1;
                if (writesRegister(in.op)) rat[in.rd] = tag;
                dyn(dynId).unitName = rs.name;
                eventos += "  ISSUE: I" + to_string(dynId) + " PC" + to_string(pc) + " " + opName(in.op) + " -> " + rs.name + " " + tag + "\n";
            } else if (in.op == Op::MUL || in.op == Op::DIV) {
                RSEntry& rs = rsMul[slot];
                rs = RSEntry{};
                rs.name = "Mult" + to_string(slot + 1);
                rs.busy = true;
                rs.op = in.op;
                rs.destTag = tag;
                rs.dynId = dynId;
                Operand a = readOperand(in.rs);
                Operand b = readOperand(in.rt);
                rs.vj = a.value; rs.qj = a.q;
                rs.vk = b.value; rs.qk = b.q;
                rs.readyCycle = (rs.qj.empty() && rs.qk.empty()) ? ciclo : -1;
                rat[in.rd] = tag;
                dyn(dynId).unitName = rs.name;
                eventos += "  ISSUE: I" + to_string(dynId) + " PC" + to_string(pc) + " " + opName(in.op) + " -> " + rs.name + " " + tag + "\n";
            } else if (in.op == Op::LOAD) {
                LSEntry& lb = loadBuf[slot];
                lb = LSEntry{};
                lb.name = "Load" + to_string(slot + 1);
                lb.busy = true;
                lb.isLoad = true;
                lb.destTag = tag;
                lb.dynId = dynId;
                lb.offset = in.offset;
                Operand base = readOperand(in.rs);
                lb.baseV = base.value; lb.baseQ = base.q;
                lb.readyCycle = (lb.baseQ.empty()) ? ciclo : -1;
                rat[in.rd] = tag;
                dyn(dynId).unitName = lb.name;
                eventos += "  ISSUE: I" + to_string(dynId) + " PC" + to_string(pc) + " LOAD -> " + lb.name + " " + tag + "\n";
            } else if (in.op == Op::STORE) {
                LSEntry& sb = storeBuf[slot];
                sb = LSEntry{};
                sb.name = "Store" + to_string(slot + 1);
                sb.busy = true;
                sb.isLoad = false;
                sb.destTag = tag;
                sb.dynId = dynId;
                sb.offset = in.offset;
                Operand base = readOperand(in.rs);
                Operand val = readOperand(in.rt);
                sb.baseV = base.value; sb.baseQ = base.q;
                sb.valueV = val.value; sb.valueQ = val.q;
                sb.readyCycle = (sb.baseQ.empty() && sb.valueQ.empty()) ? ciclo : -1;
                dyn(dynId).unitName = sb.name;
                eventos += "  ISSUE: I" + to_string(dynId) + " PC" + to_string(pc) + " STORE -> " + sb.name + " " + tag + "\n";
            }

            int oldPc = pc;
            if (in.op == Op::BNE || in.op == Op::BEQ) {
                if (cfg.branchMode == "Stall") {
                    branchBlocked = true;
                    pc = oldPc + 1;
                    emitidas++;
                    totalIssued++;
                    break;
                } else {
                    pc = oldPc + 1;
                }
            } else {
                pc = oldPc + 1;
            }

            emitidas++;
            totalIssued++;
        }

        if (emitidas == 0) ciclosSemIssue++;
        issueSlotsVazios += (cfg.issueWidth - emitidas);
        if (!motivoParada.empty()) eventos += "  ISSUE stall: " + motivoParada + "\n";
    }

    bool rsReady(const RSEntry& e) const {
        return e.busy && !e.executing && !e.waitingWB && e.qj.empty() && e.qk.empty() && e.readyCycle >= 0 && e.readyCycle < ciclo;
    }

    bool lsReadyBasic(const LSEntry& e) const {
        if (!e.busy || e.executing || e.waitingWB || e.completedStore) return false;
        if (!e.baseQ.empty()) return false;
        if (!e.isLoad && !e.valueQ.empty()) return false;
        if (e.readyCycle < 0 || e.readyCycle >= ciclo) return false;
        return true;
    }

    LSEntry* storeEntryByDyn(int dynId) {
        for (auto& s : storeBuf) if (s.busy && s.dynId == dynId) return &s;
        return nullptr;
    }

    bool loadCanExecute(LSEntry& ld, int& result, string& reason) {
        if (!lsReadyBasic(ld) || !ld.isLoad) return false;
        int addr = (ld.baseV + ld.offset) & ~3;
        int bestStoreId = -1;
        int bestStoreValue = 0;

        ROBEntry* loadRob = robByDyn(ld.dynId);
        if (!loadRob) return false;

        for (const ROBEntry& e : rob) {
            if (e.id >= loadRob->id) break;
            if (e.op != Op::STORE || e.squashed) continue;
            LSEntry* st = storeEntryByDyn(e.id);
            if (!st) continue; 

            if (!st->baseQ.empty()) {
                reason = "store antigo com endereco desconhecido";
                return false;
            }
            int stAddr = (st->baseV + st->offset) & ~3;
            if (stAddr == addr) {
                if (!st->valueQ.empty()) {
                    reason = "store antigo no mesmo endereco sem valor pronto";
                    return false;
                }
                if (e.id > bestStoreId) {
                    bestStoreId = e.id;
                    bestStoreValue = st->valueV;
                }
            }
        }

        if (bestStoreId >= 0) result = bestStoreValue;
        else result = lerMemoria(addr);
        ld.address = addr;
        ld.addressKnown = true;
        return true;
    }

    int lerMemoria(int addr) const {
        auto it = memoria.find(addr);
        return it == memoria.end() ? 0 : it->second;
    }

    void executar() {
       
        assignRS(fuAdd, rsAdd, [](Op op){ return op == Op::ADD || op == Op::SUB; });
        assignRS(fuMul, rsMul, [](Op op){ return op == Op::MUL || op == Op::DIV; });
        assignRS(fuBR, rsAdd, [](Op op){ return op == Op::BNE || op == Op::BEQ; });
        assignLS();

       
        advanceFUGroup(fuAdd);
        advanceFUGroup(fuMul);
        advanceFUGroup(fuBR);
        advanceFUGroup(fuLS);
    }

    template <typename Pred>
    void assignRS(vector<FU>& fus, vector<RSEntry>& entries, Pred pred) {
        for (FU& fu : fus) {
            if (fu.busy) continue;
            int chosen = -1;
            int bestDyn = 1e9;
            for (int i = 0; i < (int)entries.size(); i++) {
                const RSEntry& e = entries[i];
                if (!rsReady(e) || !pred(e.op)) continue;
                if (dyn(e.dynId).issue == ciclo) continue;
                if (e.dynId < bestDyn) { bestDyn = e.dynId; chosen = i; }
            }
            if (chosen >= 0) {
                RSEntry& e = entries[chosen];
                e.executing = true;
                fu.busy = true;
                fu.rs = true;
                fu.index = chosen;
                fu.dynId = e.dynId;
                fu.remaining = latency(e.op);
                if (dyn(e.dynId).start < 0) dyn(e.dynId).start = ciclo;
                eventos += "  EXEC inicio: I" + to_string(e.dynId) + " " + fu.name + "\n";
            }
        }
    }

    void assignLS() {
        for (FU& fu : fuLS) {
            if (fu.busy) continue;
            int chosenType = -1; 
            int chosen = -1;
            int bestDyn = 1e9;
            int dummyResult = 0;
            string reason;

            for (int i = 0; i < (int)loadBuf.size(); i++) {
                LSEntry& e = loadBuf[i];
                if (!e.busy || dyn(e.dynId).issue == ciclo) continue;
                int r = 0; string why;
                if (!loadCanExecute(e, r, why)) continue;
                if (e.dynId < bestDyn) { bestDyn = e.dynId; chosen = i; chosenType = 0; dummyResult = r; reason = why; }
            }
            for (int i = 0; i < (int)storeBuf.size(); i++) {
                LSEntry& e = storeBuf[i];
                if (!lsReadyBasic(e) || dyn(e.dynId).issue == ciclo) continue;
                if (e.dynId < bestDyn) { bestDyn = e.dynId; chosen = i; chosenType = 1; }
            }

            if (chosen >= 0) {
                LSEntry& e = (chosenType == 0 ? loadBuf[chosen] : storeBuf[chosen]);
                (void)dummyResult;
                (void)reason;
                e.executing = true;
                fu.busy = true;
                fu.rs = false;
                fu.index = chosen;
                fu.dynId = e.dynId;
                fu.remaining = cfg.loadStoreCycles;
                if (dyn(e.dynId).start < 0) dyn(e.dynId).start = ciclo;
                eventos += "  EXEC inicio: I" + to_string(e.dynId) + " " + fu.name + "\n";
            }
        }
    }

    void advanceFUGroup(vector<FU>& fus) {
        for (FU& fu : fus) {
            if (!fu.busy) continue;
            fu.remaining--;
            if (fu.remaining > 0) continue;

            if (fu.rs) finishRS(fu);
            else finishLS(fu);
            fu.busy = false;
            fu.index = -1;
            fu.dynId = -1;
            fu.remaining = 0;
        }
    }

    void finishRS(FU& fu) {
        RSEntry* e = nullptr;
        if (fu.kind == "MUL") e = &rsMul[fu.index];
        else e = &rsAdd[fu.index];
        if (!e->busy || e->dynId != fu.dynId) return;

        int res = 0;
        bool taken = false;
        int target = -1;
        const Instrucao& in = dyn(e->dynId).inst;

        switch (e->op) {
            case Op::ADD: res = e->vj + e->vk; break;
            case Op::SUB: res = e->vj - e->vk; break;
            case Op::MUL: res = e->vj * e->vk; break;
            case Op::DIV: res = (e->vk == 0 ? 0 : e->vj / e->vk); break;
            case Op::BNE:
                taken = (e->vj != e->vk);
                target = max(0, min((int)programa.size(), dyn(e->dynId).pc + 1 + in.offset));
                break;
            case Op::BEQ:
                taken = (e->vj == e->vk);
                target = max(0, min((int)programa.size(), dyn(e->dynId).pc + 1 + in.offset));
                break;
            default: break;
        }

        e->executing = false;
        e->waitingWB = true;
        e->result = res;
        e->branchTaken = taken;
        e->branchTarget = target;
        dyn(e->dynId).end = ciclo;
        eventos += "  EXEC fim: I" + to_string(e->dynId) + " " + fu.name + "\n";

        pendingWB.push_back(PendingWB{ciclo, e->dynId, e->destTag, e->op, res, taken, target});
    }

    void finishLS(FU& fu) {
        LSEntry& e = (fu.index >= 0 && fu.index < (int)loadBuf.size() && loadBuf[fu.index].busy && loadBuf[fu.index].dynId == fu.dynId)
            ? loadBuf[fu.index]
            : storeBuf[fu.index];

        if (!e.busy || e.dynId != fu.dynId) return;
        e.executing = false;
        dyn(e.dynId).end = ciclo;

        if (e.isLoad) {
            int result = 0;
            string why;
            bool ok = loadCanExecute(e, result, why);
            if (!ok) {
                
                dyn(e.dynId).end = -1;
                e.executing = false;
                eventos += "  EXEC abortado: I" + to_string(e.dynId) + " LOAD (" + why + ")\n";
                return;
            }
            e.result = result;
            e.waitingWB = true;
            eventos += "  EXEC fim: I" + to_string(e.dynId) + " LOAD resultado=" + to_string(result) + "\n";
            pendingWB.push_back(PendingWB{ciclo, e.dynId, e.destTag, Op::LOAD, result, false, -1});
        } else {
            e.address = (e.baseV + e.offset) & ~3;
            e.addressKnown = true;
            e.completedStore = true;
            ROBEntry* r = robByDyn(e.dynId);
            if (r) {
                r->storeAddr = e.address;
                r->storeValue = e.valueV;
                r->addressReady = true;
                r->storeValueReady = true;
                r->ready = true;
            }
           
            eventos += "  EXEC fim: I" + to_string(e.dynId) + " STORE addr=" + to_string(e.address) + " val=" + to_string(e.valueV) + "\n";
        }
    }

    void escreverCDB() {
        sort(pendingWB.begin(), pendingWB.end(), [](const PendingWB& a, const PendingWB& b) {
            if (a.readyCycle != b.readyCycle) return a.readyCycle < b.readyCycle;
            return a.dynId < b.dynId;
        });

        int wrote = 0;
        vector<PendingWB> remaining;
        for (const PendingWB& wb : pendingWB) {
            if (wrote >= cfg.cdbWidth || wb.readyCycle >= ciclo) {
                remaining.push_back(wb);
                continue;
            }
            if (dyn(wb.dynId).squashed) continue;

            ROBEntry* r = robByDyn(wb.dynId);
            if (!r) continue;

            r->ready = true;
            r->value = wb.result;
            if (wb.op == Op::BNE || wb.op == Op::BEQ) {
                r->branchTaken = wb.branchTaken;
                r->branchTarget = wb.branchTarget;
            }

            dyn(wb.dynId).wb = ciclo;
            broadcast(wb.tag, wb.result);
            freeProducerEntry(wb.dynId);

            if (wb.op == Op::BNE || wb.op == Op::BEQ) resolveBranch(*r);

            eventos += "  CDB: I" + to_string(wb.dynId) + " " + wb.tag;
            if (wb.op == Op::BNE || wb.op == Op::BEQ) {
                eventos += string(" -> ") + (wb.branchTaken ? "TAKEN" : "NOT TAKEN") + " alvo=" + to_string(wb.branchTarget) + "\n";
            } else {
                eventos += " => " + to_string(wb.result) + "\n";
            }
            wrote++;
        }
        pendingWB.swap(remaining);
    }

    void broadcast(const string& tag, int value) {
        for (auto& e : rsAdd) {
            if (!e.busy) continue;
            if (e.qj == tag) { e.qj.clear(); e.vj = value; }
            if (e.qk == tag) { e.qk.clear(); e.vk = value; }
            if (e.qj.empty() && e.qk.empty() && e.readyCycle == -1) e.readyCycle = ciclo;
        }
        for (auto& e : rsMul) {
            if (!e.busy) continue;
            if (e.qj == tag) { e.qj.clear(); e.vj = value; }
            if (e.qk == tag) { e.qk.clear(); e.vk = value; }
            if (e.qj.empty() && e.qk.empty() && e.readyCycle == -1) e.readyCycle = ciclo;
        }
        for (auto& e : loadBuf) {
            if (!e.busy) continue;
            if (e.baseQ == tag) { e.baseQ.clear(); e.baseV = value; }
            if (e.baseQ.empty() && e.readyCycle == -1) e.readyCycle = ciclo;
        }
        for (auto& e : storeBuf) {
            if (!e.busy) continue;
            if (e.baseQ == tag) { e.baseQ.clear(); e.baseV = value; }
            if (e.valueQ == tag) { e.valueQ.clear(); e.valueV = value; }
            if (e.baseQ.empty() && e.valueQ.empty() && e.readyCycle == -1) e.readyCycle = ciclo;
        }
    }

    void freeProducerEntry(int dynId) {
        for (auto& e : rsAdd) if (e.busy && e.dynId == dynId) { string name = e.name; e = RSEntry{}; e.name = name; return; }
        for (auto& e : rsMul) if (e.busy && e.dynId == dynId) { string name = e.name; e = RSEntry{}; e.name = name; return; }
        for (auto& e : loadBuf) if (e.busy && e.dynId == dynId) { string name = e.name; e = LSEntry{}; e.name = name; e.isLoad = true; return; }
    }

    void resolveBranch(const ROBEntry& br) {
        int correctPC = br.branchTaken ? br.branchTarget : br.pc + 1;
        if (cfg.branchMode == "Stall") {
            pc = correctPC;
            branchBlocked = false;
            return;
        }

        if (correctPC != br.predictedNextPC) {
            eventos += "  FLUSH: branch I" + to_string(br.id) + " errou predicao; PC=" + to_string(correctPC) + "\n";
            flushAfterBranch(br.id, br.ratSnapshot, correctPC);
        }
    }

    void flushAfterBranch(int branchDynId, const vector<string>& ratSnapshot, int correctPC) {
        for (auto& d : historico) {
            if (d.id > branchDynId && !d.squashed && d.commit < 0) {
                d.squashed = true;
                totalSquashed++;
            }
        }

        auto clearRS = [&](vector<RSEntry>& entries) {
            for (auto& e : entries) {
                if (e.busy && e.dynId > branchDynId) {
                    string name = e.name;
                    e = RSEntry{};
                    e.name = name;
                }
            }
        };
        clearRS(rsAdd);
        clearRS(rsMul);

        for (auto& e : loadBuf) {
            if (e.busy && e.dynId > branchDynId) { string name = e.name; e = LSEntry{}; e.name = name; e.isLoad = true; }
        }
        for (auto& e : storeBuf) {
            if (e.busy && e.dynId > branchDynId) { string name = e.name; e = LSEntry{}; e.name = name; e.isLoad = false; }
        }

        auto clearFU = [&](vector<FU>& fus) {
            for (auto& fu : fus) {
                if (fu.busy && fu.dynId > branchDynId) {
                    fu.busy = false;
                    fu.index = -1;
                    fu.dynId = -1;
                    fu.remaining = 0;
                }
            }
        };
        clearFU(fuAdd);
        clearFU(fuMul);
        clearFU(fuLS);
        clearFU(fuBR);

        vector<PendingWB> keep;
        for (const auto& wb : pendingWB) if (wb.dynId <= branchDynId) keep.push_back(wb);
        pendingWB.swap(keep);

        rob.erase(remove_if(rob.begin(), rob.end(), [&](const ROBEntry& e) {
            return e.id > branchDynId;
        }), rob.end());

        rat = ratSnapshot;
        for (string& tag : rat) {
            if (!tag.empty() && robByTag(tag) == nullptr) tag.clear();
        }
        pc = correctPC;
    }

    void commitROB() {
        int count = 0;
        while (count < cfg.commitWidth && !rob.empty()) {
            ROBEntry& head = rob.front();
            if (!head.ready) break;

            if (head.op == Op::STORE) {
                memoria[head.storeAddr] = head.storeValue;
                freeStoreEntry(head.id);
                eventos += "  COMMIT: I" + to_string(head.id) + " STORE Mem[" + to_string(head.storeAddr) + "]=" + to_string(head.storeValue) + "\n";
            } else if (writesRegister(head.op)) {
                if (head.destReg >= 0) {
                    regs[head.destReg].value = head.value;
                    if (rat[head.destReg] == head.tag) rat[head.destReg].clear();
                }
                eventos += "  COMMIT: I" + to_string(head.id) + " " + head.tag + "\n";
            } else if (head.op == Op::BNE || head.op == Op::BEQ) {
                eventos += "  COMMIT: I" + to_string(head.id) + " BRANCH\n";
            }

            dyn(head.id).commit = ciclo;
            totalCommitted++;
            rob.pop_front();
            count++;
        }
    }

    void freeStoreEntry(int dynId) {
        for (auto& e : storeBuf) {
            if (e.busy && e.dynId == dynId) {
                string name = e.name;
                e = LSEntry{};
                e.name = name;
                e.isLoad = false;
                return;
            }
        }
    }

    bool concluido() const {
        if (pc >= 0 && pc < (int)programa.size()) return false;
        if (branchBlocked) return false;
        if (!rob.empty()) return false;
        if (!pendingWB.empty()) return false;
        auto anyRS = [](const vector<RSEntry>& v){ for (const auto& e : v) if (e.busy) return true; return false; };
        auto anyLS = [](const vector<LSEntry>& v){ for (const auto& e : v) if (e.busy) return true; return false; };
        if (anyRS(rsAdd) || anyRS(rsMul) || anyLS(loadBuf) || anyLS(storeBuf)) return false;
        auto anyFU = [](const vector<FU>& v){ for (const auto& f : v) if (f.busy) return true; return false; };
        if (anyFU(fuAdd) || anyFU(fuMul) || anyFU(fuLS) || anyFU(fuBR)) return false;
        return true;
    }

    int remainingForDyn(int dynId) const {
        auto findFU = [&](const vector<FU>& fus) {
            for (const auto& f : fus) if (f.busy && f.dynId == dynId) return f.remaining;
            return -1;
        };
        int r;
        if ((r = findFU(fuAdd)) >= 0) return r;
        if ((r = findFU(fuMul)) >= 0) return r;
        if ((r = findFU(fuLS)) >= 0) return r;
        if ((r = findFU(fuBR)) >= 0) return r;
        return -1;
    }

    static string fmtInt(int x) { return x < 0 ? string("-") : to_string(x); }

    void mostrarEstado() const {
        cout << "=== SIMULADOR DE TOMASULO SUPERESCALAR COM ROB === Ciclo " << ciclo << " ===\n";
        cout << "PC atual: " << pc << " -> ";
        if (pc >= 0 && pc < (int)programa.size()) cout << instrToString(programa[pc]);
        else cout << "fim do programa";
        if (branchBlocked) cout << " | branch pendente";
        cout << "\n";
        cout << "IssueWidth=" << cfg.issueWidth << " CDBWidth=" << cfg.cdbWidth << " CommitWidth=" << cfg.commitWidth
             << " ROB=" << rob.size() << "/" << cfg.robEntries << " BranchMode=" << cfg.branchMode << "\n";

        cout << "\n--- Status das Instrucoes Dinamicas ---\n";
        cout << left << setw(6) << "ID" << setw(5) << "PC" << setw(30) << "Instrucao"
             << right << setw(7) << "Issue" << setw(7) << "IniEx" << setw(7) << "FimEx" << setw(7) << "CDB" << setw(8) << "Commit" << "  Estado\n";
        cout << string(92, '-') << "\n";
        int from = max(0, (int)historico.size() - 30);
        for (int i = from; i < (int)historico.size(); i++) {
            const DynInstr& d = historico[i];
            cout << left << setw(6) << ("I" + to_string(d.id)) << setw(5) << d.pc << setw(30) << instrToString(d.inst)
                 << right << setw(7) << fmtInt(d.issue) << setw(7) << fmtInt(d.start) << setw(7) << fmtInt(d.end)
                 << setw(7) << fmtInt(d.wb) << setw(8) << fmtInt(d.commit) << "  "
                 << (d.squashed ? "SQUASH" : (d.commit >= 0 ? "OK" : "")) << "\n";
        }

        cout << "\n--- Estacoes de Reserva (janela distribuida) ---\n";
        cout << left << setw(8) << "Nome" << setw(9) << "Busy" << setw(7) << "Op" << right << setw(7) << "Vj" << setw(7) << "Vk"
             << "  " << left << setw(10) << "Qj" << setw(10) << "Qk" << setw(8) << "Dest" << right << setw(6) << "Rest" << "\n";
        cout << string(78, '-') << "\n";
        auto printRS = [&](const RSEntry& e) {
            int rest = e.busy ? remainingForDyn(e.dynId) : -1;
            cout << left << setw(8) << e.name << setw(9) << (e.busy ? "Sim" : "Nao") << setw(7) << (e.busy ? opName(e.op) : "")
                 << right << setw(7) << (e.busy && e.qj.empty() ? to_string(e.vj) : "")
                 << setw(7) << (e.busy && e.qk.empty() ? to_string(e.vk) : "")
                 << "  " << left << setw(10) << (e.busy ? e.qj : "") << setw(10) << (e.busy ? e.qk : "")
                 << setw(8) << (e.busy ? e.destTag : "") << right << setw(6) << (rest >= 0 ? to_string(rest) : "") << "\n";
        };
        for (const auto& e : rsAdd) printRS(e);
        for (const auto& e : rsMul) printRS(e);

        cout << "\n--- Buffers Load/Store ---\n";
        cout << left << setw(8) << "Nome" << setw(9) << "Busy" << setw(7) << "Tipo" << right << setw(10) << "Endereco"
             << "  " << left << setw(10) << "Qbase" << setw(10) << "Qval" << right << setw(8) << "Valor" << "  "
             << left << setw(8) << "Dest" << right << setw(6) << "Rest" << "\n";
        cout << string(86, '-') << "\n";
        auto printLS = [&](const LSEntry& e) {
            int rest = e.busy ? remainingForDyn(e.dynId) : -1;
            string addr = "";
            if (e.busy && e.baseQ.empty()) addr = to_string(e.baseV) + "+" + to_string(e.offset);
            cout << left << setw(8) << e.name << setw(9) << (e.busy ? "Sim" : "Nao") << setw(7) << (e.busy ? (e.isLoad ? "LOAD" : "STORE") : "")
                 << right << setw(10) << addr << "  "
                 << left << setw(10) << (e.busy ? e.baseQ : "") << setw(10) << (e.busy && !e.isLoad ? e.valueQ : "")
                 << right << setw(8) << (e.busy && !e.isLoad && e.valueQ.empty() ? to_string(e.valueV) : (e.busy && e.isLoad && e.waitingWB ? to_string(e.result) : ""))
                 << "  " << left << setw(8) << (e.busy ? e.destTag : "") << right << setw(6) << (rest >= 0 ? to_string(rest) : "") << "\n";
        };
        for (const auto& e : loadBuf) printLS(e);
        for (const auto& e : storeBuf) printLS(e);

        cout << "\n--- ROB (commit em ordem) ---\n";
        cout << left << setw(8) << "Tag" << setw(6) << "ID" << setw(27) << "Instrucao" << setw(7) << "Dest" << setw(8) << "Ready" << setw(10) << "Valor" << setw(10) << "End" << "\n";
        cout << string(76, '-') << "\n";
        for (const auto& e : rob) {
            cout << left << setw(8) << e.tag << setw(6) << ("I" + to_string(e.id)) << setw(27) << e.instrText
                 << setw(7) << (e.destReg >= 0 ? regName(e.destReg) : "-") << setw(8) << (e.ready ? "Sim" : "Nao")
                 << setw(10) << (e.ready && writesRegister(e.op) ? to_string(e.value) : (e.op == Op::STORE && e.ready ? to_string(e.storeValue) : ""))
                 << setw(10) << (e.op == Op::STORE && e.addressReady ? to_string(e.storeAddr) : "") << "\n";
        }

        cout << "\n--- Estado dos Registradores (valor arquitetural + Qi/RAT) ---\n";
        cout << left << setw(8) << "Reg" << right << setw(8) << "Valor" << "  " << left << setw(12) << "Qi" << "\n";
        cout << string(30, '-') << "\n";
        for (int i = 0; i < cfg.numRegs; i++) {
            cout << left << setw(8) << regName(i) << right << setw(8) << regs[i].value << "  " << left << setw(12) << rat[i] << "\n";
        }

        cout << "\n--- Memoria ---\n";
        if (memoria.empty()) cout << "  vazia; leitura de endereco nao inicializado retorna 0\n";
        else for (const auto& p : memoria) cout << "  Mem[" << p.first << "] = " << p.second << "\n";

        cout << "\n--- Eventos do ciclo " << ciclo << " ---\n";
        if (eventos.empty()) cout << "  (nenhum evento)\n";
        else cout << eventos;
    }

    void mostrarResumoFinal() const {
        cout << "\nRegistradores usados no programa:\n";
        for (int r : regsUsados) {
            cout << "  " << regName(r) << " = " << regs[r].value << "\n";
        }
        cout << "\nTodos os registradores:\n";
        for (int i = 0; i < cfg.numRegs; i++) cout << "  " << regName(i) << " = " << regs[i].value << "\n";

        cout << "\nMemoria final:\n";
        if (memoria.empty()) cout << "  vazia\n";
        else for (const auto& p : memoria) cout << "  Mem[" << p.first << "] = " << p.second << "\n";

        int committed = 0;
        for (const auto& d : historico) if (!d.squashed && d.commit >= 0) committed++;
        cout << "\nResumo:\n";
        cout << "  Instrucoes emitidas = " << totalIssued << "\n";
        cout << "  Instrucoes commitadas = " << committed << "\n";
        cout << "  Instrucoes descartadas por flush = " << totalSquashed << "\n";
        cout << "  Ciclos = " << ciclo << "\n";
        if (committed > 0) {
            cout << fixed << setprecision(2);
            cout << "  CPI = " << ciclo << "/" << committed << " = " << ((double)ciclo / committed) << "\n";
        }
        cout << "  Slots de issue vazios = " << issueSlotsVazios << "\n";
        cout << "  Ciclos sem issue = " << ciclosSemIssue << "\n";
    }

    static void limparTela() {
        cout << "\033[2J\033[H" << flush;
    }
};

int main(int argc, char* argv[]) {
    string arquivo = "source.txt";
    bool passoAPasso = true;
    int maxCiclos = 0;

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--auto") passoAPasso = false;
        else if (arg == "--max" && i + 1 < argc) maxCiclos = toInt(argv[++i], "max ciclos");
        else arquivo = arg;
    }

    try {
        Tomasulo sim;
        sim.carregarArquivo(arquivo);
        sim.simular(passoAPasso, maxCiclos);
    } catch (const exception& e) {
        cerr << "Erro: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}