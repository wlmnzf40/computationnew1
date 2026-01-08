/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "CPGAnnotation.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Basic/SourceManager.h"
#include "clang/AST/ParentMapContext.h"

#include <queue>
#include <sstream>
#include <algorithm>

namespace cpg {

// ============================================
// ICFGNode实现
// ============================================

std::string ICFGNode::GetLabel() const
{
    std::ostringstream oss;
    switch (kind) {
        case ICFGNodeKind::Entry:
            oss << "Entry: " << (func ? func->getNameAsString() : "?");
            break;
        case ICFGNodeKind::Exit:
            oss << "Exit: " << (func ? func->getNameAsString() : "?");
            break;
        case ICFGNodeKind::CallSite:
            oss << "Call: " << (callee ? callee->getNameAsString() : "?");
            break;
        case ICFGNodeKind::ReturnSite:
            oss << "Return from: " << (callee ? callee->getNameAsString() : "?");
            break;
        case ICFGNodeKind::FormalIn:
            oss << "FormalIn[" << paramIndex << "]";
            if (!paramName.empty()) {
                oss << ": " << paramName;
            }
            break;
        case ICFGNodeKind::FormalOut:
            oss << "FormalOut[" << paramIndex << "]";
            if (!paramName.empty()) {
                oss << ": " << paramName;
            }
            break;
        case ICFGNodeKind::ActualIn:
            oss << "ActualIn[" << paramIndex << "]";
            if (!paramName.empty()) {
                oss << ": " << paramName;
            }
            break;
        case ICFGNodeKind::ActualOut:
            oss << "ActualOut[" << paramIndex << "]";
            if (!paramName.empty()) {
                oss << ": " << paramName;
            }
            break;
        case ICFGNodeKind::Statement:
            if (stmt) {
                oss << stmt->getStmtClassName();
            }
            break;
    }
    return oss.str();
}

void ICFGNode::Dump(const clang::SourceManager* SM) const
{
    llvm::outs() << "[ICFGNode] " << GetLabel();
    if (stmt && SM) {
        clang::PresumedLoc loc = SM->getPresumedLoc(stmt->getBeginLoc());
        if (loc.isValid()) {
            llvm::outs() << " @Line:" << loc.getLine();
        }
    }
    llvm::outs() << "\n";

    if (!successors.empty()) {
        llvm::outs() << "  Successors: ";
        for (const auto& [succ, kind] : successors) {
            llvm::outs() << succ->GetLabel() << " (";
            switch (kind) {
                case ICFGEdgeKind::Intraprocedural: llvm::outs() << "intra"; break;
                case ICFGEdgeKind::Call: llvm::outs() << "call"; break;
                case ICFGEdgeKind::Return: llvm::outs() << "ret"; break;
                case ICFGEdgeKind::ParamIn: llvm::outs() << "pin"; break;
                case ICFGEdgeKind::ParamOut: llvm::outs() << "pout"; break;
                case ICFGEdgeKind::True: llvm::outs() << "T"; break;
                case ICFGEdgeKind::False: llvm::outs() << "F"; break;
                case ICFGEdgeKind::Unconditional: llvm::outs() << "ε"; break;
            }
            llvm::outs() << "), ";
        }
        llvm::outs() << "\n";
    }
}

// ============================================
// PDGNode实现
// ============================================

void PDGNode::Dump(const clang::SourceManager* sm) const
{
    llvm::outs() << "[PDGNode] ";
    if (stmt) {
        llvm::outs() << stmt->getStmtClassName();
        if (sm) {
            clang::PresumedLoc loc = sm->getPresumedLoc(stmt->getBeginLoc());
            if (loc.isValid()) {
                llvm::outs() << " @Line:" << loc.getLine();
            }
        }
    }
    llvm::outs() << "\n";

    if (!dataDeps.empty()) {
        llvm::outs() << "  Data Dependencies:\n";
        for (const auto& dep : dataDeps) {
            llvm::outs() << "    " << dep.varName << " <- ";
            switch (dep.kind) {
                case DataDependency::DepKind::Flow: llvm::outs() << "Flow"; break;
                case DataDependency::DepKind::Anti: llvm::outs() << "Anti"; break;
                case DataDependency::DepKind::Output: llvm::outs() << "Output"; break;
            }
            llvm::outs() << "\n";
        }
    }

    if (!controlDeps.empty()) {
        llvm::outs() << "  Control Dependencies:\n";
        for (const auto& dep : controlDeps) {
            llvm::outs() << "    Controlled by: " << dep.controlStmt->getStmtClassName()
                        << " [" << (dep.branchValue ? "T" : "F") << "]\n";
        }
    }
}

// ============================================
// CallContext实现
// ============================================

std::string CallContext::ToString() const
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < callStack.size(); ++i) {
        if (i > 0) {
            oss << " -> ";
        }
    }
    oss << "]";
    return oss.str();
}

// ============================================
// PathCondition实现
// ============================================

bool PathCondition::IsFeasible() const
{
    return true;
}

std::string PathCondition::ToString() const
{
    std::ostringstream oss;
    oss << "Path[";
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << (conditions[i].second ? "T" : "F");
    }
    oss << "]";
    return oss.str();
}

// ============================================
// CPGContext基础实现
// ============================================

CPGContext::CPGContext(clang::ASTContext& ctx) : astContext(ctx) {}

// ---------- ICFG接口实现 ----------

ICFGNode* CPGContext::GetICFGNode(const clang::Stmt* stmt) const
{
    auto it = stmtToICFGNode.find(stmt);
    return it != stmtToICFGNode.end() ? it->second : nullptr;
}

ICFGNode* CPGContext::GetFunctionEntry(const clang::FunctionDecl* func) const
{
    if (!func) {
        return nullptr;
    }
    // 【修复】使用规范化指针查找
    auto it = funcEntries.find(func->getCanonicalDecl());
    return it != funcEntries.end() ? it->second : nullptr;
}

ICFGNode* CPGContext::GetFunctionExit(const clang::FunctionDecl* func) const
{
    if (!func) {
        return nullptr;
    }
    // 【修复】使用规范化指针查找
    auto it = funcExits.find(func->getCanonicalDecl());
    return it != funcExits.end() ? it->second : nullptr;
}

std::vector<ICFGNode*> CPGContext::GetSuccessors(ICFGNode* node) const
{
    std::vector<ICFGNode*> result;
    for (const auto& [succ, _] : node->successors) {
        result.push_back(succ);
    }
    return result;
}

std::vector<ICFGNode*> CPGContext::GetPredecessors(ICFGNode* node) const
{
    std::vector<ICFGNode*> result;
    for (const auto& [pred, _] : node->predecessors) {
        result.push_back(pred);
    }
    return result;
}

std::vector<std::pair<ICFGNode*, ICFGEdgeKind>> CPGContext::GetSuccessorsWithEdgeKind(
    ICFGNode* node) const
{
    return node->successors;
}

// ---------- PDG接口实现 ----------

PDGNode* CPGContext::GetPDGNode(const clang::Stmt* stmt) const
{
    auto it = pdgNodes.find(stmt);
    return it != pdgNodes.end() ? it->second.get() : nullptr;
}

std::vector<DataDependency> CPGContext::GetDataDependencies(
    const clang::Stmt* stmt) const
{
    auto* node = GetPDGNode(stmt);
    return node ? node->dataDeps : std::vector<DataDependency>();
}

std::vector<ControlDependency> CPGContext::GetControlDependencies(
    const clang::Stmt* stmt) const
{
    auto* node = GetPDGNode(stmt);
    return node ? node->controlDeps : std::vector<ControlDependency>();
}

std::set<const clang::Stmt*> CPGContext::GetDefinitions(
    const clang::Stmt* useStmt, const std::string& varName) const
{
    auto* func = GetContainingFunction(useStmt);
    if (!func) {
        return {};
    }

    auto it = reachingDefsMap.find(func);
    if (it == reachingDefsMap.end()) {
        return {};
    }

    const auto& reachInfo = it->second;
    auto reachIt = reachInfo.reachingDefs.find(useStmt);
    if (reachIt == reachInfo.reachingDefs.end()) {
        return {};
    }

    auto varIt = reachIt->second.find(varName);
    if (varIt == reachIt->second.end()) {
        return {};
    }

    return varIt->second;
}

std::set<const clang::Stmt*> CPGContext::GetUses(
    const clang::Stmt* defStmt, const std::string& varName) const
{
    std::set<const clang::Stmt*> uses;
    for (const auto& [stmt, node] : pdgNodes) {
        for (const auto& dep : node->dataDeps) {
            if (dep.sourceStmt == defStmt && dep.varName == varName) {
                uses.insert(stmt);
            }
        }
    }
    return uses;
}

// ---------- 路径查询实现 ----------

// 辅助函数：将变量的所有使用点加入worklist
void CPGContext::EnqueueVariableUses(
    const clang::Stmt* current,
    const std::string& var,
    std::queue<const clang::Stmt*>& worklist,
    std::set<const clang::Stmt*>& visited) const
{
    auto uses = GetUses(current, var);
    for (auto* use : uses) {
        if (!visited.count(use)) {
            worklist.push(use);
            visited.insert(use);
        }
    }
}

// 辅助函数：处理当前语句定义的变量
void CPGContext::ProcessDefinedVarsForPath(
    const clang::Stmt* current,
    const std::string& varName,
    std::queue<const clang::Stmt*>& worklist,
    std::set<const clang::Stmt*>& visited) const
{
    auto definedVars = GetDefinedVars(current);
    for (const auto& var : definedVars) {
        if (!varName.empty() && var != varName) {
            continue;
        }
        EnqueueVariableUses(current, var, worklist, visited);
    }
}

// 主函数：现在非常简洁
bool CPGContext::HasDataFlowPath(
    const clang::Stmt* source,
    const clang::Stmt* sink,
    const std::string& varName) const
{
    std::queue<const clang::Stmt*> worklist;
    std::set<const clang::Stmt*> visited;

    worklist.push(source);
    visited.insert(source);

    while (!worklist.empty()) {
        auto* current = worklist.front();
        worklist.pop();

        if (current == sink) {
            return true;
        }

        ProcessDefinedVarsForPath(current, varName, worklist, visited);
    }
    return false;
}

bool CPGContext::HasControlFlowPath(const clang::Stmt* source,
    const clang::Stmt* sink) const
{
    auto* sourceNode = GetICFGNode(source);
    auto* sinkNode = GetICFGNode(sink);

    if (!sourceNode || !sinkNode) {
        return false;
    }

    std::queue<ICFGNode*> worklist;
    std::set<ICFGNode*> visited;

    worklist.push(sourceNode);
    visited.insert(sourceNode);

    while (!worklist.empty()) {
        auto* current = worklist.front();
        worklist.pop();

        if (current == sinkNode) {
            return true;
        }

        for (auto* succ : GetSuccessors(current)) {
            if (visited.find(succ) == visited.end()) {
                worklist.push(succ);
                visited.insert(succ);
            }
        }
    }
    return false;
}

// 辅助函数：遍历后继节点
void CPGContext::ExploreSuccessors(
    ICFGNode* node,
    ICFGNode* sink,
    int depth,
    int maxDepth,
    std::vector<ICFGNode*>& currentPath,
    std::set<ICFGNode*>& visited,
    std::vector<std::vector<ICFGNode*>>& allPaths) const
{
    for (auto* succ : GetSuccessors(node)) {
        if (!visited.count(succ)) {
            FindPathsDFS(succ, sink, depth + 1, maxDepth,
                         currentPath, visited, allPaths);
        }
    }
}

// 辅助函数：DFS 遍历实现
void CPGContext::FindPathsDFS(
    ICFGNode* node,
    ICFGNode* sink,
    int depth,
    int maxDepth,
    std::vector<ICFGNode*>& currentPath,
    std::set<ICFGNode*>& visited,
    std::vector<std::vector<ICFGNode*>>& allPaths) const
{
    if (depth > maxDepth) {
        return;
    }

    currentPath.push_back(node);
    visited.insert(node);

    if (node == sink) {
        allPaths.push_back(currentPath);
    } else {
        ExploreSuccessors(node, sink, depth, maxDepth,
                          currentPath, visited, allPaths);
    }

    visited.erase(node);
    currentPath.pop_back();
}

// 主函数：现在非常简洁
std::vector<std::vector<ICFGNode*>> CPGContext::FindAllPaths(
    ICFGNode* source,
    ICFGNode* sink,
    int maxDepth) const
{
    std::vector<std::vector<ICFGNode*>> allPaths;
    std::vector<ICFGNode*> currentPath;
    std::set<ICFGNode*> visited;

    FindPathsDFS(source, sink, 0, maxDepth, currentPath, visited, allPaths);
    return allPaths;
}

// ---------- 辅助功能实现 ----------

const clang::FunctionDecl* CPGContext::GetContainingFunction(
    const clang::Stmt* stmt) const
{
    for (const auto& [func, nodes] : icfgNodes) {
        for (const auto& node : nodes) {
            if (node->stmt == stmt) {
                return func;
            }
        }
    }
    return nullptr;
}

const clang::CFG* CPGContext::GetCFG(const clang::FunctionDecl* func) const
{
    if (!func) return nullptr;
    // 【修复】使用规范化指针查找
    auto it = cfgCache.find(func->getCanonicalDecl());
    return it != cfgCache.end() ? it->second.get() : nullptr;
}

// ---------- Dump方法实现 ----------

void CPGContext::DumpICFG(const clang::FunctionDecl* func) const
{
    llvm::outs() << "\n========== ICFG: " << func->getNameAsString()
                 << " ==========\n";

    // 【修复】使用规范化指针查找
    auto it = icfgNodes.find(func->getCanonicalDecl());
    if (it == icfgNodes.end()) {
        llvm::outs() << "No ICFG found\n";
        return;
    }

    const clang::SourceManager& SM = astContext.getSourceManager();
    for (const auto& node : it->second) {
        node->Dump(&SM);
    }

    llvm::outs() << "===============================================\n\n";
}

void CPGContext::DumpPDG(const clang::FunctionDecl* func) const
{
    llvm::outs() << "\n========== PDG: " << func->getNameAsString()
                 << " ==========\n";

    int count = 0;
    const clang::SourceManager& SM = astContext.getSourceManager();
    for (const auto& [stmt, node] : pdgNodes) {
        if (GetContainingFunction(stmt) == func) {
            llvm::outs() << "[" << count++ << "] ";
            node->Dump(&SM);
        }
    }

    llvm::outs() << "===============================================\n\n";
}

void CPGContext::DumpCPG(const clang::FunctionDecl* func) const
{
    llvm::outs() << "\n========== CPG: " << func->getNameAsString()
                 << " ==========\n";
    DumpICFG(func);
    DumpPDG(func);
}

void CPGContext::DumpNode(ICFGNode* node) const
{
    if (node) {
        const clang::SourceManager& SM = astContext.getSourceManager();
        node->Dump(&SM);
    }
}

void CPGContext::DumpNode(PDGNode* node) const
{
    if (node) {
        const clang::SourceManager& SM = astContext.getSourceManager();
        node->Dump(&SM);
    }
}

void CPGContext::PrintStatistics() const
{
    llvm::outs() << "\n=== CPG Statistics ===\n";

    int totalICFGNodes = 0;
    for (const auto& [_, nodes] : icfgNodes) {
        totalICFGNodes += nodes.size();
    }

    llvm::outs() << "Functions: " << icfgNodes.size() << "\n";
    llvm::outs() << "ICFG nodes: " << totalICFGNodes << "\n";
    llvm::outs() << "PDG nodes: " << pdgNodes.size() << "\n";
    llvm::outs() << "Cached CFGs: " << cfgCache.size() << "\n";
    llvm::outs() << "======================\n\n";
}

// 辅助函数：从赋值语句收集使用的变量
void CPGContext::CollectUsedVarsFromAssignment(
    const clang::BinaryOperator* binOp,
    std::set<std::string>& vars) const
{
    // 收集右侧表达式的变量
    if (auto* rhs = binOp->getRHS()) {
        VarCollector rhsCollector(vars);
        rhsCollector.TraverseStmt(const_cast<clang::Expr*>(rhs));
    }

    // 对于复合赋值（+=, -=, *= 等），左侧变量也是被使用的
    if (binOp->isCompoundAssignmentOp()) {
        auto* lhs = llvm::dyn_cast<clang::DeclRefExpr>(
            binOp->getLHS()->IgnoreParenImpCasts());
        if (!lhs) return;

        auto* var = llvm::dyn_cast<clang::VarDecl>(lhs->getDecl());
        if (var) {
            vars.insert(var->getNameAsString());
        }
    }
}

// 辅助函数：从声明语句收集使用的变量
void CPGContext::CollectUsedVarsFromDeclStmt(
    const clang::DeclStmt* declStmt,
    std::set<std::string>& vars) const
{
    for (auto* decl : declStmt->decls()) {
        auto* varDecl = llvm::dyn_cast<clang::VarDecl>(decl);
        if (!varDecl) continue;

        auto* init = varDecl->getInit();
        if (init) {
            VarCollector initCollector(vars);
            initCollector.TraverseStmt(const_cast<clang::Expr*>(init));
        }
    }
}

// 主函数：获取语句中使用的变量
std::set<std::string> CPGContext::GetUsedVars(const clang::Stmt* stmt) const
{
    std::set<std::string> vars;

    // 处理赋值语句
    if (auto* binOp = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
        if (binOp->isAssignmentOp()) {
            CollectUsedVarsFromAssignment(binOp, vars);
            return vars;
        }
    }

    // 处理声明语句
    if (auto* declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        CollectUsedVarsFromDeclStmt(declStmt, vars);
        return vars;
    }

    // 其他语句：收集所有变量引用
    VarCollector collector(vars);
    collector.TraverseStmt(const_cast<clang::Stmt*>(stmt));
    return vars;
}

// 辅助函数：从赋值操作中提取被定义的变量
void CPGContext::ExtractDefinedVarFromAssignment(
    const clang::BinaryOperator* binOp,
    std::set<std::string>& vars) const
{
    if (!binOp->isAssignmentOp()) {
        return;
    }

    auto* lhs = llvm::dyn_cast<clang::DeclRefExpr>(
        binOp->getLHS()->IgnoreParenImpCasts());
    if (!lhs) {
        return;
    }

    auto* var = llvm::dyn_cast<clang::VarDecl>(lhs->getDecl());
    if (!var) {
        return;
    }

    vars.insert(var->getNameAsString());
}

// 辅助函数：从声明语句中提取被定义的变量
void CPGContext::ExtractDefinedVarsFromDeclStmt(
    const clang::DeclStmt* declStmt,
    std::set<std::string>& vars) const
{
    for (auto* decl : declStmt->decls()) {
        auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
        if (var) {
            vars.insert(var->getNameAsString());
        }
    }
}

// 辅助函数：从一元自增自减操作中提取被定义的变量（++i, i++, --i, i--）
void CPGContext::ExtractDefinedVarFromUnaryOp(
    const clang::UnaryOperator* unaryOp,
    std::set<std::string>& vars) const
{
    clang::UnaryOperatorKind opKind = unaryOp->getOpcode();
    if (opKind != clang::UO_PreInc && opKind != clang::UO_PostInc &&     // 只处理自增自减操作
        opKind != clang::UO_PreDec && opKind != clang::UO_PostDec) {
        return;
    }

    const auto* subExpr = unaryOp->getSubExpr();
    if (!subExpr) {
        return;
    }

    const auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(
        subExpr->IgnoreParenImpCasts());
    if (!declRef) {
        return;
    }

    const auto* var = llvm::dyn_cast<clang::VarDecl>(declRef->getDecl());
    if (!var) {
        return;
    }

    vars.insert(var->getNameAsString());
}

std::set<std::string> CPGContext::GetDefinedVars(const clang::Stmt* stmt) const
{
    std::set<std::string> vars;

    if (const auto* binOp = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
        ExtractDefinedVarFromAssignment(binOp, vars);
    } else if (const auto* declStmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        ExtractDefinedVarsFromDeclStmt(declStmt, vars);
    } else if (const auto* unaryOp = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
        ExtractDefinedVarFromUnaryOp(unaryOp, vars);
    }

    return vars;
}

// ============================================
// 【新增】从缓存直接获取（高效版本）
// ============================================

std::set<std::string> CPGContext::GetUsedVarsCached(const clang::Stmt* stmt) const
{
    if (!stmt) {
        return {};
    }

    // 获取包含该语句的函数
    auto* func = GetContainingFunction(stmt);
    if (!func) {
        // 如果不在函数中，回退到重新计算
        return GetUsedVars(stmt);
    }

    // 从缓存中查找
    auto it = reachingDefsMap.find(func);
    if (it == reachingDefsMap.end()) {
        // 缓存未建立，回退到重新计算
        return GetUsedVars(stmt);
    }

    const auto& reachInfo = it->second;
    auto usesIt = reachInfo.uses.find(stmt);
    if (usesIt != reachInfo.uses.end()) {
        return usesIt->second;  // 返回缓存的结果
    }

    // 缓存中没有，回退到重新计算
    return GetUsedVars(stmt);
}

std::set<std::string> CPGContext::GetDefinedVarsCached(const clang::Stmt* stmt) const
{
    if (!stmt) {
        return {};
    }

    // 获取包含该语句的函数
    auto* func = GetContainingFunction(stmt);
    if (!func) {
        return GetDefinedVars(stmt);
    }

    // 从缓存中查找
    auto it = reachingDefsMap.find(func);
    if (it == reachingDefsMap.end()) {
        return GetDefinedVars(stmt);
    }

    const auto& reachInfo = it->second;
    auto defsIt = reachInfo.definitions.find(stmt);
    if (defsIt != reachInfo.definitions.end()) {
        return defsIt->second;  // 返回缓存的结果
    }

    return GetDefinedVars(stmt);
}

std::string CPGContext::GetStmtSource(const clang::Stmt* stmt) const
{
    unsigned long limitLen = 50;
    unsigned long shadowLen = 3;

    if (!stmt) {
        return "<null>";
    }

    clang::SourceRange range = stmt->getSourceRange();
    if (range.isInvalid()) {
        return "<invalid>";
    }

    clang::CharSourceRange charRange =
        clang::CharSourceRange::getTokenRange(range);
    std::string source = clang::Lexer::getSourceText(
        charRange,
        astContext.getSourceManager(),
        astContext.getLangOpts()
    ).str();

    std::replace(source.begin(), source.end(), '\n', ' ');
    std::replace(source.begin(), source.end(), '\t', ' ');

    if (source.length() > limitLen) {
        source = source.substr(0, limitLen - shadowLen) + "...";
    }

    return source;
}

std::string CPGContext::EscapeForDot(const std::string& str) const
{
    std::string result;
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '<': result += "\\<"; break;
            case '>': result += "\\>"; break;
            case '{': result += "\\{"; break;
            case '}': result += "\\}"; break;
            case '|': result += "\\|"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::set<std::string> CPGContext::ExtractVariables(const clang::Expr* expr) const
{
    std::set<std::string> vars;
    VarExtractor extractor(vars);
    extractor.TraverseStmt(const_cast<clang::Expr*>(expr));
    return vars;
}

const clang::Stmt* CPGContext::GetContainingStmt(const clang::Expr* expr) const
{
    if (!expr) {
        return nullptr;
    }

    auto parents = astContext.getParents(*expr);

    while (!parents.empty()) {
        const auto& parent = parents[0];

        if (auto* stmt = parent.get<clang::Stmt>()) {
            if (stmtToICFGNode.count(stmt)) {
                return stmt;
            }
            parents = astContext.getParents(*stmt);
        } else {
            break;
        }
    }

    return nullptr;
}
} // namespace cpg