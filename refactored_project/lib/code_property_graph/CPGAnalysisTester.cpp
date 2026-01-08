/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "code_property_graph/CPGAnalysisTester.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>
#include <set>

using namespace clang;
using namespace llvm;

// ============================================
// SectionPrinter 实现
// ============================================

void SectionPrinter::PrintHeader(const std::string& title)
{
    size_t lineLen = 66;
    outs() << "\n";
    outs() << "╔══════════════════════════════════════════════════════════════════╗\n";
    outs() << "║ " << title;
    for (size_t i = title.length(); i < lineLen; ++i) {
        outs() << " ";
    }
    outs() << "║\n";
    outs() << "╚══════════════════════════════════════════════════════════════════╝\n";
}

void SectionPrinter::PrintSubHeader(const std::string& title)
{
    size_t lineLen = 64;
    outs() << "\n┌─────────────────────────────────────────────────────────────────┐\n";
    outs() << "│ " << title;
    for (size_t i = title.length(); i < lineLen; ++i) {
        outs() << " ";
    }
    outs() << "│\n";
    outs() << "└─────────────────────────────────────────────────────────────────┘\n";
}

void SectionPrinter::PrintSeparator()
{
    outs() << "───────────────────────────────────────────────────────────────────\n";
}

// ============================================
// 辅助函数实现
// ============================================

void PrintEdgeKind(cpg::ICFGEdgeKind kind)
{
    switch (kind) {
        case cpg::ICFGEdgeKind::Intraprocedural: outs() << "intra"; break;
        case cpg::ICFGEdgeKind::Call: outs() << "call"; break;
        case cpg::ICFGEdgeKind::Return: outs() << "return"; break;
        case cpg::ICFGEdgeKind::ParamIn: outs() << "param_in"; break;
        case cpg::ICFGEdgeKind::ParamOut: outs() << "param_out"; break;
        case cpg::ICFGEdgeKind::True: outs() << "true"; break;
        case cpg::ICFGEdgeKind::False: outs() << "false"; break;
        case cpg::ICFGEdgeKind::Unconditional: outs() << "unconditional"; break;
    }
}

// ============================================
// ICFGTester 实现
// ============================================

void ICFGTester::TestFeatures(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    outs() << "\n[Testing ICFG Features]\n";
    TestEntryExitNodes(func, cpgCtx);
    TestSuccessorsPredecessors(func, cpgCtx);
    TestCFGRetrieval(func, cpgCtx);
}

void ICFGTester::TestEntryExitNodes(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto* entry = cpgCtx.GetFunctionEntry(func);
    auto* exit = cpgCtx.GetFunctionExit(func);

    if (entry) {
        outs() << "  Entry node found: " << entry->GetLabel() << "\n";
        if (g_config.verbose) {
            cpgCtx.DumpNode(entry);
        }
    } else {
        outs() << "   Entry node not found!\n";
    }

    if (exit) {
        outs() << "  Exit node found: " << exit->GetLabel() << "\n";
        if (g_config.verbose) {
            cpgCtx.DumpNode(exit);
        }
    } else {
        outs() << "  Exit node not found!\n";
    }
}

void ICFGTester::TestSuccessorsPredecessors(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto* entry = cpgCtx.GetFunctionEntry(func);
    auto* exit = cpgCtx.GetFunctionExit(func);

    if (entry) {
        auto successors = cpgCtx.GetSuccessors(entry);
        outs() << "  Entry successors count: " << successors.size() << "\n";

        auto succWithKind = cpgCtx.GetSuccessorsWithEdgeKind(entry);
        outs() << "  Entry successors with edge kinds:\n";
        for (const auto& [succ, kind] : succWithKind) {
            outs() << "      -> " << succ->GetLabel() << " (";
            PrintEdgeKind(kind);
            outs() << ")\n";
        }
    }

    if (exit) {
        auto predecessors = cpgCtx.GetPredecessors(exit);
        outs() << "  Exit predecessors count: " << predecessors.size() << "\n";
    }
}

void ICFGTester::TestCFGRetrieval(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto* cfg = cpgCtx.GetCFG(func);
    if (cfg) {
        outs() << "  CFG retrieved, blocks: " << cfg->size() << "\n";
    }
}

// ============================================
// PDGTester 实现
// ============================================

void PDGTester::TestFeatures(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    outs() << "\n[Testing PDG Features]\n";

    int pdgNodeCount = 0;
    int dataDepCount = 0;
    int controlDepCount = 0;

    CountDependencies(func, cpgCtx, pdgNodeCount, dataDepCount, controlDepCount);

    outs() << "  PDG nodes created: " << pdgNodeCount << "\n";
    outs() << "  Data dependencies found: " << dataDepCount << "\n";
    outs() << "  Control dependencies found: " << controlDepCount << "\n";
}

void PDGTester::CountDependencies(FunctionDecl* func, cpg::CPGContext& cpgCtx,
    int& pdgNodes, int& dataDeps, int& controlDeps)
{
    class StmtVisitor : public RecursiveASTVisitor<StmtVisitor> {
    public:
        cpg::CPGContext& ctx;
        int& pdgNodes;
        int& dataDeps;
        int& controlDeps;

        StmtVisitor(cpg::CPGContext& c, int& pn, int& dd, int& cd)
            : ctx(c), pdgNodes(pn), dataDeps(dd), controlDeps(cd) {}

        bool VisitStmt(Stmt* stmt)
        {
            auto* pdgNode = ctx.GetPDGNode(stmt);
            if (pdgNode) {
                pdgNodes++;
                dataDeps += pdgNode->dataDeps.size();
                controlDeps += pdgNode->controlDeps.size();
                if (g_config.verbose) {
                    ctx.DumpNode(pdgNode);
                }
            }
            return true;
        }
    };

    StmtVisitor visitor(cpgCtx, pdgNodes, dataDeps, controlDeps);
    visitor.TraverseStmt(func->getBody());
}

// ============================================
// DataFlowTester 实现
// ============================================

class DefUseCollector : public RecursiveASTVisitor<DefUseCollector> {
public:
    std::vector<std::pair<const Stmt*, std::string>>& defs;
    std::vector<std::pair<const Expr*, std::string>>& uses;

    DefUseCollector(std::vector<std::pair<const Stmt*, std::string>>& d,
                    std::vector<std::pair<const Expr*, std::string>>& u)
        : defs(d), uses(u) {}

    bool VisitBinaryOperator(BinaryOperator* binOp)
    {
        if (binOp->isAssignmentOp()) {
            if (auto* lhs = dyn_cast<DeclRefExpr>(binOp->getLHS()->IgnoreParenImpCasts())) {
                if (auto* var = dyn_cast<VarDecl>(lhs->getDecl())) {
                    defs.push_back({binOp, var->getNameAsString()});
                }
            }
        }
        return true;
    }

    bool VisitDeclStmt(DeclStmt* declStmt)
    {
        for (auto* decl : declStmt->decls()) {
            if (auto* var = dyn_cast<VarDecl>(decl)) {
                defs.push_back({declStmt, var->getNameAsString()});
            }
        }
        return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr* ref)
    {
        if (auto* var = dyn_cast<VarDecl>(ref->getDecl())) {
            uses.push_back({ref, var->getNameAsString()});
        }
        return true;
    }
};

void DataFlowTester::TestAnalysis(FunctionDecl* func, cpg::CPGContext& cpgCtx,
    ASTContext& astContext)
{
    outs() << "\n[Testing Data Flow Analysis]\n";

    std::vector<std::pair<const Stmt*, std::string>> definitions;
    std::vector<std::pair<const Expr*, std::string>> uses;

    CollectDefsAndUses(func, definitions, uses);

    outs() << "  Found " << definitions.size() << " definitions\n";
    outs() << "  Found " << uses.size() << " uses\n";

    TestDefinitionsAndUses(definitions, uses, cpgCtx);
    TestDataFlowPath(definitions, cpgCtx);
    TestExtractVariables(uses, cpgCtx);
}

void DataFlowTester::CollectDefsAndUses(
    FunctionDecl* func,
    std::vector<std::pair<const Stmt*, std::string>>& defs,
    std::vector<std::pair<const Expr*, std::string>>& uses)
{
    DefUseCollector collector(defs, uses);
    collector.TraverseStmt(func->getBody());
}

void DataFlowTester::TestDefinitionsAndUses(
    const std::vector<std::pair<const Stmt*, std::string>>& defs,
    const std::vector<std::pair<const Expr*, std::string>>& uses,
    cpg::CPGContext& cpgCtx)
{
    for (const auto& [defStmt, varName] : defs) {
        auto usesFound = cpgCtx.GetUses(defStmt, varName);
        if (g_config.verbose || !usesFound.empty()) {
            outs() << "  Variable '" << varName << "' defined, "
                   << usesFound.size() << " uses found\n";
        }
    }

    int maxDepth = 5;
    for (const auto& [useExpr, varName] : uses) {
        auto defChain = cpgCtx.TraceVariableDefinitions(useExpr, maxDepth);
        if (g_config.verbose && !defChain.empty()) {
            outs() << "  Traced " << defChain.size()
                   << " definitions for '" << varName << "'\n";
        }
    }
}

void DataFlowTester::TestDataFlowPath(
    const std::vector<std::pair<const Stmt*, std::string>>& defs,
    cpg::CPGContext& cpgCtx)
{
    size_t limitedSize = 2;
    if (defs.size() >= limitedSize) {
        auto& [src, srcVar] = defs[0];
        auto& [sink, sinkVar] = defs.back();

        bool hasPath = cpgCtx.HasDataFlowPath(src, sink, "");
        outs() << "  Data flow path test: "
               << (hasPath ? "exists" : "not found") << "\n";
    }
}

void DataFlowTester::TestExtractVariables(
    const std::vector<std::pair<const Expr*, std::string>>& uses,
    cpg::CPGContext& cpgCtx)
{
    if (!uses.empty()) {
        auto vars = cpgCtx.ExtractVariables(uses[0].first);
        outs() << "  ExtractVariables test: found "
               << vars.size() << " variables\n";
    }
}

// ============================================
// ControlFlowTester 实现
// ============================================

void ControlFlowTester::TestAnalysis(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    outs() << "\n[Testing Control Flow Analysis]\n";
    TestPathExistence(func, cpgCtx);
    TestFindAllPaths(func, cpgCtx);
    TestControlStatements(func, cpgCtx);
}

void ControlFlowTester::TestPathExistence(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto* entry = cpgCtx.GetFunctionEntry(func);
    auto* exit = cpgCtx.GetFunctionExit(func);
    if (entry && exit) {
        auto entrySuccs = cpgCtx.GetSuccessors(entry);
        if (!entrySuccs.empty() && entrySuccs[0]->stmt) {
            auto exitPreds = cpgCtx.GetPredecessors(exit);
            if (!exitPreds.empty() && exitPreds[0]->stmt) {
                bool hasPath = cpgCtx.HasControlFlowPath(
                    entrySuccs[0]->stmt, exitPreds[0]->stmt);
                outs() << "  Control flow path: "
                       << (hasPath ? "exists" : "not found") << "\n";
            }
        }
    }
}

void ControlFlowTester::TestFindAllPaths(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto* entry = cpgCtx.GetFunctionEntry(func);
    auto* exit = cpgCtx.GetFunctionExit(func);

    if (entry && exit) {
        auto paths = cpgCtx.FindAllPaths(entry, exit, 20);
        outs() << "  FindAllPaths: found " << paths.size()
               << " paths (depth limit: 20)\n";
    }
}

void ControlFlowTester::TestControlStatements(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    int condStmtCount = 0;

    class Counter : public RecursiveASTVisitor<Counter> {
    public:
        int& count;
        explicit Counter(int& c) : count(c) {}
        bool VisitIfStmt(IfStmt*)
        {
            count++;
            return true;
        }
        bool VisitWhileStmt(WhileStmt*)
        {
            count++;
            return true;
        }
        bool VisitForStmt(ForStmt*)
        {
            count++;
            return true;
        }
    };

    Counter counter(condStmtCount);
    counter.TraverseStmt(func->getBody());
    outs() << "  Control statements found: " << condStmtCount << "\n";
}

// ============================================
// InterproceduralTester 实现
// ============================================

void InterproceduralTester::TestAnalysis(
    const std::vector<FunctionDecl*>& funcs,
    cpg::CPGContext& cpgCtx)
{
    SectionPrinter::PrintSubHeader("Interprocedural Analysis Tests");
    TestCallGraphTraversal(funcs, cpgCtx);
    TestInterproceduralDataFlow(funcs, cpgCtx);
    TestForwardSlicing(funcs, cpgCtx);
}

void InterproceduralTester::TestCallGraphTraversal(
    const std::vector<FunctionDecl*>& funcs,
    cpg::CPGContext& cpgCtx)
{
    int maxDepth = 5;
    outs() << "[Testing Call Graph Traversal]\n";

    for (auto* func : funcs) {
        cpgCtx.TraverseCallGraphContextSensitive(
            func,
            [](const FunctionDecl* f, const cpg::CallContext& ctx) {
                outs() << "  Visited: " << f->getNameAsString()
                       << " Context: " << ctx.ToString() << "\n";
            },
            maxDepth);
    }
}

void InterproceduralTester::TestCallArgumentTrace(const CallExpr* call, cpg::CPGContext& cpgCtx)
{
    if (call->getNumArgs() == 0) {
        return;
    }

    auto* arg = cpgCtx.GetArgumentAtCallSite(call, 0);
    if (!arg) {
        return;
    }

    outs() << "  Argument at call site found\n";
    auto defChain = cpgCtx.TraceVariableDefinitionsInterprocedural(arg, 5);
    outs() << "  Interprocedural backward trace: "
           << defChain.size() << " definitions\n";
}

void InterproceduralTester::TestCalleeParameterUsages(const CallExpr* call, cpg::CPGContext& cpgCtx)
{
    auto* callee = call->getDirectCallee();
    if (!callee || callee->param_size() == 0) {
        return;
    }

    auto usages = cpgCtx.GetParameterUsages(callee->getParamDecl(0));
    outs() << "  Parameter usages in "
           << callee->getNameAsString()
           << ": " << usages.size() << "\n";
}

void InterproceduralTester::TestSingleCall(const CallExpr* call, cpg::CPGContext& cpgCtx)
{
    TestCallArgumentTrace(call, cpgCtx);
    TestCalleeParameterUsages(call, cpgCtx);
}

std::vector<const CallExpr*> InterproceduralTester::CollectCallExprs(FunctionDecl* func)
{
    class CallFinder : public RecursiveASTVisitor<CallFinder> {
    public:
        std::vector<const CallExpr*> calls;
        bool VisitCallExpr(CallExpr* call)
        {
            calls.push_back(call);
            return true;
        }
    };

    CallFinder finder;
    finder.TraverseStmt(func->getBody());
    return finder.calls;
}

void InterproceduralTester::TestFunctionInterproceduralFlow(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto calls = CollectCallExprs(func);
    for (const auto* call : calls) {
        TestSingleCall(call, cpgCtx);
    }
}

void InterproceduralTester::TestInterproceduralDataFlow(
    const std::vector<FunctionDecl*>& funcs,
    cpg::CPGContext& cpgCtx)
{
    outs() << "\n[Testing Interprocedural Data Flow]\n";
    for (auto* func : funcs) {
        TestFunctionInterproceduralFlow(func, cpgCtx);
    }
}

InterproceduralTester::FirstDefinitionInfo InterproceduralTester::ExtractDefinitionFromAssignment(
    BinaryOperator* binOp)
{
    FirstDefinitionInfo info;

    if (!binOp->isAssignmentOp()) {
        return info;
    }

    auto* lhs = dyn_cast<DeclRefExpr>(binOp->getLHS()->IgnoreParenImpCasts());
    if (!lhs) {
        return info;
    }

    auto* var = dyn_cast<VarDecl>(lhs->getDecl());
    if (!var) {
        return info;
    }

    info.stmt = binOp;
    info.varName = var->getNameAsString();
    return info;
}

InterproceduralTester::FirstDefinitionInfo InterproceduralTester::FindFirstDefinition(
    FunctionDecl* func)
{
    class FirstDefFinder : public RecursiveASTVisitor<FirstDefFinder> {
    public:
        FirstDefinitionInfo info;

        bool VisitBinaryOperator(BinaryOperator* binOp)
        {
            if (info.stmt) {
                return true;
            }
            info = ExtractDefinitionFromAssignment(binOp);
            return true;
        }
    };

    FirstDefFinder finder;
    finder.TraverseStmt(func->getBody());
    return finder.info;
}

void InterproceduralTester::TestFunctionForwardSlice(FunctionDecl* func, cpg::CPGContext& cpgCtx)
{
    auto defInfo = FindFirstDefinition(func);
    if (!defInfo.stmt) {
        return;
    }

    auto uses = cpgCtx.TraceVariableUsesInterprocedural(defInfo.stmt, defInfo.varName, 5);
    outs() << "  Forward slice from '" << defInfo.varName
           << "' in " << func->getNameAsString()
           << ": " << uses.size() << " uses\n";
}

void InterproceduralTester::TestForwardSlicing(
    const std::vector<FunctionDecl*>& funcs,
    cpg::CPGContext& cpgCtx)
{
    outs() << "\n[Testing Forward Interprocedural Slicing]\n";
    for (auto* func : funcs) {
        TestFunctionForwardSlice(func, cpgCtx);
    }
}

// ============================================
// CPGBuilderTester 实现
// ============================================

void CPGBuilderTester::TestBuildForTranslationUnit(ASTContext& context)
{
    cpg::CPGContext newCtx(context);
    cpg::CPGBuilder::BuildForTranslationUnit(context, newCtx);
    outs() << "CPGBuilder::BuildForTranslationUnit completed\n";
}

void CPGBuilderTester::TestBuildForFunction(ASTContext& context)
{
    for (auto* decl : context.getTranslationUnitDecl()->decls()) {
        if (auto* func = dyn_cast<FunctionDecl>(decl)) {
            if (func->hasBody() && func->isThisDeclarationADefinition()) {
                cpg::CPGContext singleCtx(context);
                cpg::CPGBuilder::BuildForFunction(func, singleCtx);
                outs() << "CPGBuilder::BuildForFunction for "
                       << func->getNameAsString() << " completed\n";
                break;
            }
        }
    }
}

// ============================================
// UtilityClassTester 实现
// ============================================

void UtilityClassTester::TestCallContext()
{
    cpg::CallContext ctx1;
    cpg::CallContext ctx2;
    outs() << "CallContext created: " << ctx1.ToString() << "\n";
    outs() << "CallContext comparison: "
           << (ctx1 == ctx2 ? "equal" : "not equal") << "\n";
}

void UtilityClassTester::TestPathCondition()
{
    cpg::PathCondition path;
    path.AddCondition(nullptr, true);
    path.AddCondition(nullptr, false);
    outs() << "PathCondition created: " << path.ToString() << "\n";
    outs() << "PathCondition feasible: "
           << (path.IsFeasible() ? "yes" : "no") << "\n";
}
