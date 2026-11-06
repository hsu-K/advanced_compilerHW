#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <string>
#include <cmath>

using namespace llvm;

// 一次陣列存取：Array[A*i + B]，屬於第 Stmt 個 statement
struct Access {
    StringRef Array;
    int Stmt;
    int64_t A, B;
    bool IsWrite;
};

// 把 V 表示成 A*i + B（i 是 IVar 這個 alloca 的值），成功回傳 true
static bool getAffine(Value *V, Value *IVar, int64_t &A, int64_t &B) {
    if (auto *CI = dyn_cast<ConstantInt>(V)) {
        A = 0;
        B = CI->getSExtValue();
        return true;
    }
    if (auto *LD = dyn_cast<LoadInst>(V)) {
        if (LD->getPointerOperand() != IVar)
            return false;
        A = 1;
        B = 0;
        return true;
    }
    if (auto *Cast = dyn_cast<SExtInst>(V))
        return getAffine(Cast->getOperand(0), IVar, A, B);
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
        int64_t A1, B1, A2, B2;
        if (!getAffine(BO->getOperand(0), IVar, A1, B1) ||
            !getAffine(BO->getOperand(1), IVar, A2, B2))
            return false;
        switch (BO->getOpcode()) {
        case Instruction::Add:
            A = A1 + A2;
            B = B1 + B2;
            return true;
        case Instruction::Sub:
            A = A1 - A2;
            B = B1 - B2;
            return true;
        case Instruction::Mul:
            if (A1 != 0 && A2 != 0) // i*i 不是線性


            
                return false;
            A = A1 * B2 + A2 * B1;
            B = B1 * B2;
            return true;
        default:
            return false;
        }
    }
    return false;
}

// 找迴圈變數 i：header 裡 icmp 的第一個運算元是 load %i
static Value *getInductionVar(Loop *L) {
    for (auto &I : *L->getHeader())
        if (auto *Cmp = dyn_cast<ICmpInst>(&I))
            if (auto *LD = dyn_cast<LoadInst>(Cmp->getOperand(0)))
                return LD->getPointerOperand();
    return nullptr;
}

static void printSubscript(raw_ostream &OS, int64_t A, int64_t B) {
    if (A == 0) {
        OS << B;
        return;
    }
    if (A == -1)
        OS << "-";
    else if (A != 1)
        OS << A << "*";
    OS << "i";
    if (B > 0)
        OS << " + " << B;
    else if (B < 0)
        OS << " - " << -B;
}

static int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    // 處理是負數且是小數的情況
    if (a % b != 0 && ((a < 0) != (b < 0)))
        --q;
    return q;
}

static int64_t ceilDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    // 處理是負數且是小數的情況
    if (a % b != 0 && ((a < 0) == (b < 0)))
        ++q;
    return q;
}

// 迴圈範圍：第 k 次迭代（k = 0, 1, ..., Count-1）時 i = Start + k*Step
// Step 可以是負的（例如 i--），所以 i 越大不一定越早執行
struct LoopBounds {
    int64_t Start, Step, Count;
    int64_t last() const { return Start + (Count - 1) * Step; }
    int64_t min() const { return Step > 0 ? Start : last(); }
    int64_t max() const { return Step > 0 ? last() : Start; }
    // i 的值是第幾次迭代，用來比較執行先後
    int64_t order(int64_t X) const { return (X - Start) / Step; }
    bool contains(int64_t X) const {
        return (X - Start) % Step == 0 && order(X) >= 0 && order(X) < Count;
    }
};

// 讀迴圈範圍
static bool getLoopBounds(Loop *L, Value *IVar, LoopBounds &LB) {
    // 起點：preheader 裡的 store <常數>, %i
    // store i32 4, ptr %i, align 4
    bool FoundLo = false;
    if (BasicBlock *Pre = L->getLoopPreheader()){
        for (auto &I : *Pre){
            if (auto *ST = dyn_cast<StoreInst>(&I)){
                // 確認 store 的位置是 i
                if (ST->getPointerOperand() == IVar){
                    if (auto *CI = dyn_cast<ConstantInt>(ST->getValueOperand())) {
                        LB.Start = CI->getSExtValue();
                        FoundLo = true;
                    }
                }
            }
        }
    }

    // 結束條件：header 裡的 icmp <pred> i, <常數>，統一換成「i 最後一個可能的值 Last」
    // %cmp = icmp slt i32 %0, 20   → i < 20，Last = 19
    // %cmp = icmp sge i32 %0, 4    → i >= 4，Last = 4（倒數的迴圈）
    bool FoundHi = false;
    int64_t Last = 0;
    int Dir = 0; // +1：i 要遞增；-1：i 要遞減；0：i != N，兩個方向都可以
    for (auto &I : *L->getHeader()) {
        auto *Cmp = dyn_cast<ICmpInst>(&I);
        if (!Cmp)
            continue;
        if (auto *CI = dyn_cast<ConstantInt>(Cmp->getOperand(1))) {
            int64_t N = CI->getSExtValue();
            FoundHi = true;
            // 看是大於小於等於哪種比較運算
            switch (Cmp->getPredicate()) {
            case ICmpInst::ICMP_SLT:
            case ICmpInst::ICMP_ULT:
                Last = N - 1;
                Dir = 1;
                break;
            case ICmpInst::ICMP_SLE:
            case ICmpInst::ICMP_ULE:
                Last = N;
                Dir = 1;
                break;
            case ICmpInst::ICMP_SGT:
            case ICmpInst::ICMP_UGT:
                Last = N + 1;
                Dir = -1;
                break;
            case ICmpInst::ICMP_SGE:
            case ICmpInst::ICMP_UGE:
                Last = N;
                Dir = -1;
                break;
            case ICmpInst::ICMP_NE:
                Last = N; // 等知道步長後再換成 N - Step
                Dir = 0;
                break;
            default:
                FoundHi = false;
                break;
            }
        }
        break;
    }

    // 步長：latch 裡的 store (i + Step), %i
    // store i32 %inc, ptr %i, align 4
    bool FoundStep = false;
    if (BasicBlock *Latch = L->getLoopLatch())
        for (auto &I : *Latch)
            if (auto *ST = dyn_cast<StoreInst>(&I))
                // 確認是 i
                if (ST->getPointerOperand() == IVar) {
                    int64_t A, B;
                    // i++ 是 B = 1，i-- 是 B = -1，i += 3 是 B = 3
                    if (getAffine(ST->getValueOperand(), IVar, A, B) && A == 1 && B != 0) {
                        LB.Step = B;
                        FoundStep = true;
                    }
                }

    if (!FoundLo || !FoundHi || !FoundStep)
        return false;
    if (Dir == 0) // i != N：最後一次是 N - Step
        Last -= LB.Step;
    else if ((Dir > 0) != (LB.Step > 0)) // 方向不符，例如 i < 20 卻 i--
        return false;

    // 迭代次數：Start, Start+Step, ... 不超過 Last
    LB.Count = std::max<int64_t>(floorDiv(Last - LB.Start, LB.Step) + 1, 0);
    return true;
}

// g = gcd(a, b); a*x + b*y = g（a, b >= 0）
static int64_t exgcd(int64_t a, int64_t b, int64_t &x, int64_t &y) {
    if (b == 0) {
        x = 1;
        y = 0;
        return a;
    }
    int64_t g = exgcd(b, a % b, y, x);
    y -= (a / b) * x;
    return g;
}

// 解 a*x + b*y = c，把 x、y 都在迴圈範圍內的整數解放進 Sols
static void solveDiophantine(int64_t a, int64_t b, int64_t c, const LoopBounds &LB,
                             SmallVectorImpl<std::pair<int64_t, int64_t>> &Sols) {
    if (LB.Count <= 0) // 迴圈一次都不執行
        return;
    if (a == 0 && b == 0) {
        // 兩邊索引都是常數：相等的話任兩次迭代都碰到同一元素
        if (c != 0)
            return;
        // 例如 A[3] = .... = A[3]。每次都會發生 Dependence
        for (int64_t kx = 0; kx < LB.Count; ++kx)
            for (int64_t ky = 0; ky < LB.Count; ++ky)
                Sols.push_back({LB.Start + kx * LB.Step, LB.Start + ky * LB.Step});
        return;
    }

    // 1. 解 |a|*x0 + |b|*y0 = g，再調整正負號得到 a*x0 + b*y0 = g
    int64_t x0, y0;
    int64_t g = exgcd(std::abs(a), std::abs(b), x0, y0);
    if (a < 0)
        x0 = -x0;
    if (b < 0)
        y0 = -y0;

    // 2. g 不整除 c：沒有整數解
    if (c % g != 0)
        return;

    // 3. 乘上 c/g 得到一組特解
    x0 *= c / g;
    y0 *= c / g;

    // 4. 通解：x = x0 + dx*t, y = y0 + dy*t
    int64_t dx = b / g, dy = -a / g;

    // 用 x（dx 為 0 時改用 y）限制 t 的範圍，讓它落在 [min, max]
    int64_t Base, D;
    if (dx != 0){
        Base = x0;
        D = dx;
    } else{
        Base = y0;
        D = dy;
    }

    int64_t TLo, THi;
    if (D > 0) {
        TLo = ceilDiv(LB.min() - Base, D);
        THi = floorDiv(LB.max() - Base, D);
    } else {
        TLo = ceilDiv(LB.max() - Base, D);
        THi = floorDiv(LB.min() - Base, D);
    }
    for (int64_t t = TLo; t <= THi; ++t) {
        int64_t x = x0 + dx * t, y = y0 + dy * t;
        if (LB.contains(x) && LB.contains(y))
            Sols.push_back({x, y});
    }
}

// 一筆相依：Array 在第 SrcIdx 次迭代的 S<SrcStmt> → 第 DstIdx 次迭代的 S<DstStmt>
struct Dep {
    int64_t SrcIdx, DstIdx;
    std::string Array;
    int SrcStmt, DstStmt;
    bool operator<(const Dep &O) const {
        return std::tie(SrcIdx, DstIdx, Array, SrcStmt, DstStmt) <
               std::tie(O.SrcIdx, O.DstIdx, O.Array, O.SrcStmt, O.DstStmt);
    }
};

static void printDeps(raw_ostream &OS, StringRef Title, StringRef Arrow, const std::set<Dep> &Deps) {
    OS << "====" << Title << "====\n";
    for (const Dep &D : Deps)
        OS << "(i=" << D.SrcIdx << ",i=" << D.DstIdx << ")\n"
           << D.Array << ":S" << D.SrcStmt << " " << Arrow << " S" << D.DstStmt << "\n";
}

// 用 json::OStream 依呼叫順序輸出（json::Object 印出時會把 key 依字母排序）
static void writeDeps(json::OStream &J, StringRef Key, const std::set<Dep> &Deps) {
    J.attributeArray(Key, [&] {
        for (const Dep &D : Deps)
            J.object([&] {
                J.attribute("array", D.Array);
                J.attribute("src_stmt", D.SrcStmt);
                J.attribute("src_idx", D.SrcIdx);
                J.attribute("dst_stmt", D.DstStmt);
                J.attribute("dst_idx", D.DstIdx);
            });
    });
}

struct HW1Pass : public PassInfoMixin<HW1Pass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

PreservedAnalyses HW1Pass::run(Function &F, FunctionAnalysisManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty())
        return PreservedAnalyses::all();

    std::set<Dep> Flow, Anti, Output;
    for (Loop *L : LI) {
        // 取得for迴圈的迭代值 i
        Value *IVar = getInductionVar(L);
        if (!IVar) {
            errs() << "cannot find induction variable\n";
            continue;
        }

        // 依程式順序收集陣列存取；每遇到一個陣列 store 就結束一個 statement
        // 用 SmallVector，如果數量較少就存在 stack，超過(16個)就存到 heap
        SmallVector<Access, 16> Accesses;
        int Stmt = 1;
        for (BasicBlock *BB : L->getBlocks()) {
            for (auto &I : *BB) {
                Value *Ptr;
                bool IsWrite;
                if (auto *LD = dyn_cast<LoadInst>(&I)) {
                    Ptr = LD->getPointerOperand();
                    IsWrite = false;
                } else if (auto *ST = dyn_cast<StoreInst>(&I)) {
                    Ptr = ST->getPointerOperand();
                    IsWrite = true;
                } else {
                    continue;
                }

                auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
                if (!GEP) // 純量（例如 i），不分析
                    continue;

                int64_t A, B;
                // getOperand(2) 取 %idxprom
                if (!getAffine(GEP->getOperand(2), IVar, A, B)) {
                    errs() << "unsupported subscript: " << *GEP << "\n";
                    continue;
                }
                Accesses.push_back({GEP->getPointerOperand()->getName(), Stmt, A, B, IsWrite});
                if (IsWrite)
                    ++Stmt;
            }
        }

        // 輸出 Statement
        int LastStmt = 0;
        for (const Access &Acc : Accesses) {
            if (Acc.Stmt != LastStmt) {
                errs() << "S" << Acc.Stmt << ":\n";
                LastStmt = Acc.Stmt;
            }
            errs() << "  " << (Acc.IsWrite ? "write " : "read  ") << Acc.Array << "[";
            printSubscript(errs(), Acc.A, Acc.B);
            errs() << "]\n";
        }

        LoopBounds LB;
        if (!getLoopBounds(L, IVar, LB)) {
            errs() << "cannot find loop bounds\n";
            continue;
        }
        errs() << "Loop: i = " << LB.Start << " .. " << LB.last() << ", step " << LB.Step
               << " (" << LB.Count << " iterations)\n";

        // 兩兩配對同一陣列、至少一方是寫的存取；Accesses 的順序就是本體內的執行順序
        for (size_t P = 0; P < Accesses.size(); ++P) {
            for (size_t Q = P; Q < Accesses.size(); ++Q) {
                const Access &X = Accesses[P], &Y = Accesses[Q];
                if (X.Array != Y.Array || (!X.IsWrite && !Y.IsWrite))
                    continue;

                // X 在第 x 次、Y 在第 y 次迭代碰到同一元素：X.A*x + X.B = Y.A*y + Y.B
                SmallVector<std::pair<int64_t, int64_t>, 32> Sols;
                solveDiophantine(X.A, -Y.A, Y.B - X.B, LB, Sols);

                for (auto [XI, YI] : Sols) {
                    if (P == Q && XI == YI) // 同一次存取
                        continue;
                    // 先執行的是 source：迭代較早，或同一迭代中在本體較前面
                    // 比的是第幾次迭代，不是 i 的值：倒數的迴圈裡 i 越大越早執行
                    int64_t XK = LB.order(XI), YK = LB.order(YI);
                    bool XFirst = XK < YK || (XK == YK && P < Q);
                    const Access &Src = XFirst ? X : Y, &Dst = XFirst ? Y : X;
                    Dep D{XFirst ? XI : YI, XFirst ? YI : XI, Src.Array.str(), Src.Stmt, Dst.Stmt};
                    if (Src.IsWrite && !Dst.IsWrite)
                        Flow.insert(D);
                    else if (!Src.IsWrite && Dst.IsWrite)
                        Anti.insert(D);
                    else
                        Output.insert(D);
                }
            }
        }
    }

    printDeps(errs(), "Flow Dependence", "----->", Flow);
    printDeps(errs(), "Anti-Dependence", "--A-->", Anti);
    printDeps(errs(), "Output Dependence", "--O-->", Output);

    // 輸出 <原始檔名>.json，例如 test1.c → test1.json
    std::string OutFile = (sys::path::stem(F.getParent()->getSourceFileName()) + ".json").str();
    std::error_code EC;
    raw_fd_ostream OS(OutFile, EC);
    if (EC) {
        errs() << "cannot open " << OutFile << ": " << EC.message() << "\n";
        return PreservedAnalyses::all();
    }
    json::OStream J(OS, 4); // 縮排 4 格
    J.object([&] {
        writeDeps(J, "FlowDependence", Flow);
        writeDeps(J, "AntiDependence", Anti);
        writeDeps(J, "OutputDependence", Output);
    });
    OS << "\n";

    return PreservedAnalyses::all();
}

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HW1Pass", LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                    ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "hw1") {
                        FPM.addPass(HW1Pass());
                        return true;
                    }
                    return false;
                });
            }};
}
