/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "CPGAnnotation.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>

namespace cpg {

// ============================================
// 过程内数据流追踪
// ============================================

std::vector<const clang::Stmt*> CPGContext::TraceVariableDefinitions(
    const clang::Expr* expr, int maxDepth) const
{
    std::vector<const clang::Stmt*> result;
    if (!expr) {
        return result;
    }

    auto vars = ExtractVariables(expr);
    if (vars.empty()) {
        return result;
    }

    const clang::Stmt* containingStmt = GetContainingStmt(expr);
    if (!containingStmt) {
        containingStmt = expr;
    }

    auto* func = GetContainingFunction(containingStmt);
    if (!func) return result;

    std::set<const clang::Stmt*> visited;
    std::queue<std::pair<const clang::Stmt*, int>> worklist;

    worklist.push({containingStmt, 0});
    visited.insert(containingStmt);

    for (const auto& varName : vars) {
        TraceDefinitionsForVar(varName, worklist, visited, result, maxDepth);
    }

    return result;
}

// 辅助函数：处理单个定义语句
void CPGContext::ProcessDefinitionStmt(
    const clang::Stmt* defStmt,
    int depth,
    std::queue<std::pair<const clang::Stmt*, int>>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    if (visited.count(defStmt)) {
        return;
    }

    result.push_back(defStmt);
    visited.insert(defStmt);

    // 【优化】使用缓存版本，避免重复AST遍历
    auto usedVars = GetUsedVarsCached(defStmt);
    if (!usedVars.empty()) {
        worklist.push({defStmt, depth + 1});
    }
}

// 辅助函数：处理单轮迭代
void CPGContext::ProcessDefinitionsRound(
    const clang::Stmt* current,
    int depth,
    const std::string& varName,
    std::queue<std::pair<const clang::Stmt*, int>>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    auto defs = GetDefinitions(current, varName);

    for (auto* defStmt : defs) {
        ProcessDefinitionStmt(defStmt, depth, worklist, visited, result);
    }
}

void CPGContext::TraceDefinitionsForVar(
    const std::string& varName,
    std::queue<std::pair<const clang::Stmt*, int>>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result,
    int maxDepth) const
{
    while (!worklist.empty()) {
        auto [current, depth] = worklist.front();
        worklist.pop();

        if (depth >= maxDepth) {
            continue;
        }

        ProcessDefinitionsRound(current, depth, varName,
                                worklist, visited, result);
    }
}

// ============================================
// 跨函数向后追踪
// ============================================

std::vector<const clang::Stmt*> CPGContext::TraceVariableDefinitionsInterprocedural(
    const clang::Expr* expr,
    int maxDepth) const
{
    std::vector<const clang::Stmt*> result;
    if (!expr) {
        return result;
    }

    auto vars = ExtractVariables(expr);
    if (vars.empty()) {
        return result;
    }

    const clang::Stmt* containingStmt = GetContainingStmt(expr);
    if (!containingStmt) {
        containingStmt = expr;
    }

    auto* func = GetContainingFunction(containingStmt);
    if (!func) {
        return result;
    }

    std::set<const clang::Stmt*> visited;
    std::queue<InterproceduralWorkItem> worklist;

    for (const auto& varName : vars) {
        worklist.push({containingStmt, 0, func, varName});
    }
    visited.insert(containingStmt);

    ProcessInterproceduralBackwardTrace(expr, worklist, visited, result, maxDepth);

    return result;
}

void CPGContext::ProcessInterproceduralBackwardTrace(
    const clang::Expr* expr,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result,
    int maxDepth) const
{
    while (!worklist.empty()) {
        auto [current, depth, currentFunc, varName] = worklist.front();
        worklist.pop();

        if (depth >= maxDepth) {
            continue;
        }

        ProcessLocalDefinitions(current, currentFunc, varName,
                                depth, worklist, visited, result);
        ProcessParameterBackward(expr, currentFunc, varName,
                                 depth, worklist, visited, result);
    }
}

void CPGContext::ProcessLocalDefinitions(
    const clang::Stmt* current,
    const clang::FunctionDecl* currentFunc,
    const std::string& varName,
    int depth,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    auto defs = GetDefinitions(current, varName);

    for (auto* defStmt : defs) {
        if (visited.find(defStmt) == visited.end()) {
            result.push_back(defStmt);
            visited.insert(defStmt);

            // 【优化】使用缓存版本，避免重复AST遍历
            auto usedVars = GetUsedVarsCached(defStmt);
            for (const auto& usedVar : usedVars) {
                worklist.push({defStmt, depth + 1, currentFunc, usedVar});
            }
        }
    }
}

void CPGContext::ProcessParameterBackward(
    const clang::Expr* expr,
    const clang::FunctionDecl* currentFunc,
    const std::string& varName,
    int depth,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    if (auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
        if (auto* paramDecl = llvm::dyn_cast<clang::ParmVarDecl>(DRE->getDecl())) {
            unsigned paramIndex = paramDecl->getFunctionScopeIndex();

            TraceParameterBackward(currentFunc, paramIndex, varName,
                depth, worklist, visited, result);
        }
    }
}

// 辅助函数：检查调用是否指向目标函数
bool CPGContext::IsCallToFunction(
    const clang::CallExpr* callExpr,
    const clang::FunctionDecl* targetFunc) const
{
    auto it = callTargets.find(callExpr);
    return it != callTargets.end() && it->second == targetFunc;
}

// 辅助函数：处理单个参数的回溯
void CPGContext::ProcessArgumentBackward(
    const clang::Expr* arg,
    const clang::CallExpr* callExpr,
    const clang::FunctionDecl* caller,
    int depth,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    if (!arg) {
        return;
    }

    if (!visited.count(arg)) {
        result.push_back(arg);
        visited.insert(arg);
    }

    const clang::Stmt* callStmt = GetContainingStmt(callExpr);
    if (!callStmt) {
        callStmt = callExpr;
    }

    auto argVars = ExtractVariables(arg);
    for (const auto& argVar : argVars) {
        worklist.push({callStmt, depth + 1, caller, argVar});
    }
}

// 辅助函数：处理单个调用点
void CPGContext::ProcessCallSiteBackward(
    const clang::CallExpr* callExpr,
    const clang::FunctionDecl* caller,
    const clang::FunctionDecl* currentFunc,
    unsigned paramIndex,
    int depth,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    if (!IsCallToFunction(callExpr, currentFunc)) {
        return;
    }

    const clang::Expr* arg = GetArgumentAtCallSite(callExpr, paramIndex);
    ProcessArgumentBackward(arg, callExpr, caller, depth,
                            worklist, visited, result);
}

void CPGContext::TraceParameterBackward(
    const clang::FunctionDecl* currentFunc,
    unsigned paramIndex,
    const std::string& varName,
    int depth,
    std::queue<InterproceduralWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result) const
{
    for (const auto& [caller, callExprs] : callSites) {
        for (const auto* callExpr : callExprs) {
            ProcessCallSiteBackward(callExpr, caller, currentFunc, paramIndex,
                                    depth, worklist, visited, result);
        }
    }
}

// ============================================
// 跨函数向前追踪
// ============================================

std::vector<const clang::Stmt*> CPGContext::TraceVariableUsesInterprocedural(
    const clang::Stmt* defStmt,
    const std::string& varName,
    int maxDepth) const
{
    std::vector<const clang::Stmt*> result;
    if (!defStmt) {
        return result;
    }

    std::string targetVar = varName;
    if (targetVar.empty()) {
        // 【优化】使用缓存版本
        auto definedVars = GetDefinedVarsCached(defStmt);
        if (definedVars.empty()) {
            return result;
        }
        targetVar = *definedVars.begin();
    }

    auto* func = GetContainingFunction(defStmt);
    if (!func) {
        return result;
    }

    std::set<const clang::Stmt*> visited;
    std::queue<ForwardWorkItem> worklist;

    worklist.push({defStmt, nullptr, 0, func, targetVar});

    ProcessInterproceduralForwardTrace(worklist, visited, result, maxDepth);

    return result;
}

void CPGContext::ProcessInterproceduralForwardTrace(
    std::queue<ForwardWorkItem>& worklist,
    std::set<const clang::Stmt*>& visited,
    std::vector<const clang::Stmt*>& result,
    int maxDepth) const
{
    while (!worklist.empty()) {
        auto [currentDef, currentParam, depth, currentFunc, currentVar] =
            worklist.front();
        worklist.pop();

        if (depth >= maxDepth) {
            continue;
        }

        std::vector<const clang::Stmt*> localUses;
        CollectLocalUses(currentDef, currentParam, currentVar, localUses);

        for (const auto* useStmt : localUses) {
            if (visited.find(useStmt) == visited.end()) {
                result.push_back(useStmt);
                visited.insert(useStmt);

                ProcessForwardUse(useStmt, currentVar, currentFunc,
                                  depth, worklist);
            }
        }
    }
}

void CPGContext::CollectLocalUses(
    const clang::Stmt* currentDef,
    const clang::ParmVarDecl* currentParam,
    const std::string& currentVar,
    std::vector<const clang::Stmt*>& localUses) const
{
    if (currentParam) {
        localUses = GetParameterUsages(currentParam);
    } else if (currentDef) {
        auto usesSet = GetUses(currentDef, currentVar);
        localUses.assign(usesSet.begin(), usesSet.end());
    }
}

void CPGContext::ProcessForwardUse(
    const clang::Stmt* useStmt,
    const std::string& currentVar,
    const clang::FunctionDecl* currentFunc,
    int depth,
    std::queue<ForwardWorkItem>& worklist) const
{
    if (auto* callExpr = llvm::dyn_cast<clang::CallExpr>(useStmt)) {
        ProcessForwardCallSite(callExpr, currentVar, depth, worklist);
    } else if (auto* binOp = llvm::dyn_cast<clang::BinaryOperator>(useStmt)) {
        ProcessForwardAssignment(binOp, currentVar, currentFunc, depth, worklist);
    } else if (auto* declStmt = llvm::dyn_cast<clang::DeclStmt>(useStmt)) {
        ProcessForwardDeclStmt(declStmt, currentVar, currentFunc, depth, worklist);
    }
    // 【修复】处理 UnaryOperator (++/--)
    // pVect1++ 同时使用和定义了 pVect1，需要继续追踪其后续使用
    else if (auto* unaryOp = llvm::dyn_cast<clang::UnaryOperator>(useStmt)) {
        if (unaryOp->isIncrementDecrementOp()) {
            if (auto* subExpr = unaryOp->getSubExpr()) {
                if (auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(
                        subExpr->IgnoreParenImpCasts())) {
                    std::string varName = declRef->getDecl()->getNameAsString();
                    // ++ 操作定义了新值，继续追踪这个变量的后续使用
                    worklist.push({useStmt, nullptr, depth, currentFunc, varName});
                }
            }
        }
    }
}

void CPGContext::ProcessForwardCallSite(
    const clang::CallExpr* callExpr,
    const std::string& currentVar,
    int depth,
    std::queue<ForwardWorkItem>& worklist) const
{
    unsigned argIndex = 0;
    bool foundAsArg = false;
    for (const auto* arg : callExpr->arguments()) {
        auto usedVars = ExtractVariables(arg);
        if (usedVars.count(currentVar)) {
            foundAsArg = true;
            break;
        }
        argIndex++;
    }

    if (foundAsArg) {
        if (auto* callee = callExpr->getDirectCallee()) {
            if (argIndex < callee->param_size()) {
                const clang::ParmVarDecl* param = callee->getParamDecl(argIndex);

                llvm::outs() << "发现跨函数数据流(Forward): 实参 " << currentVar
                           << " -> 形参 " << param->getNameAsString()
                           << " in " << callee->getNameAsString() << "\n";

                worklist.push({nullptr, param, depth + 1, callee,
                               param->getNameAsString()});
            }
        }
    }
}

void CPGContext::ProcessForwardAssignment(
    const clang::BinaryOperator* binOp,
    const std::string& currentVar,
    const clang::FunctionDecl* currentFunc,
    int depth,
    std::queue<ForwardWorkItem>& worklist) const
{
    if (binOp->isAssignmentOp()) {
        if (auto* lhs = llvm::dyn_cast<clang::DeclRefExpr>(
                binOp->getLHS()->IgnoreParenImpCasts())) {
            std::string newVar = lhs->getDecl()->getNameAsString();
            worklist.push({binOp, nullptr, depth, currentFunc, newVar});
        }
    }
}

    // 辅助函数：处理单个变量声明
void CPGContext::ProcessVarDeclForward(
    const clang::VarDecl* varDecl,
    const clang::DeclStmt* declStmt,
    const std::string& currentVar,
    const clang::FunctionDecl* currentFunc,
    int depth,
    std::queue<ForwardWorkItem>& worklist) const
{
    if (!varDecl || !varDecl->getInit()) {
        return;
    }

    auto initVars = ExtractVariables(varDecl->getInit());
    if (!initVars.count(currentVar)) {
        return;
    }

    worklist.push({declStmt, nullptr, depth, currentFunc,
                   varDecl->getNameAsString()});
}


void CPGContext::ProcessForwardDeclStmt(
    const clang::DeclStmt* declStmt,
    const std::string& currentVar,
    const clang::FunctionDecl* currentFunc,
    int depth,
    std::queue<ForwardWorkItem>& worklist) const
{
    for (auto* decl : declStmt->decls()) {
        auto* varDecl = llvm::dyn_cast<clang::VarDecl>(decl);
        ProcessVarDeclForward(varDecl, declStmt, currentVar,
                              currentFunc, depth, worklist);
    }
}

// ============================================
// 参数相关辅助方法
// ============================================

const clang::Expr* CPGContext::GetArgumentAtCallSite(
    const clang::CallExpr* callExpr,
    unsigned paramIndex) const
{
    if (!callExpr || paramIndex >= callExpr->getNumArgs()) {
        return nullptr;
    }
    return callExpr->getArg(paramIndex);
}

std::vector<const clang::Stmt*> CPGContext::GetParameterUsages(
    const clang::ParmVarDecl* param) const
{
    std::vector<const clang::Stmt*> usages;
    if (!param) {
        return usages;
    }

    const clang::FunctionDecl* func =
        llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
    if (!func || !func->hasBody()) {
        return usages;
    }

    ParamUsageFinder finder(param);
    finder.TraverseStmt(func->getBody());

    return finder.foundUsages;
}

// ============================================
// 上下文敏感接口实现
// ============================================

PDGNode* CPGContext::GetPDGNodeInContext(const clang::Stmt* stmt,
    const CallContext& context) const
{
    return GetPDGNode(stmt);
}

std::vector<DataDependency> CPGContext::GetDataDependenciesOnPath(
    const clang::Stmt* stmt,
    const PathCondition& path) const
{
    return GetDataDependencies(stmt);
}

// 辅助函数：处理单个调用点的上下文敏感遍历
void CPGContext::ProcessCallSiteContextSensitive(
    const clang::CallExpr* call,
    const CallContext& context,
    int depth,
    int maxDepth,
    CallGraphVisitor& visitor) const
{
    auto targetIt = callTargets.find(call);
    if (targetIt == callTargets.end()) {
        return;
    }

    CallContext newContext = context;
    newContext.callStack.push_back(call);
    TraverseCallGraphDFS(targetIt->second, newContext, depth + 1,
                         maxDepth, visitor);
}

// 辅助函数：DFS 遍历实现
void CPGContext::TraverseCallGraphDFS(
    const clang::FunctionDecl* func,
    const CallContext& context,
    int depth,
    int maxDepth,
    CallGraphVisitor& visitor) const
{
    if (depth > maxDepth) {
        return;
    }

    visitor(func, context);

    auto it = callSites.find(func);
    if (it == callSites.end()) {
        return;
    }

    for (const auto* call : it->second) {
        ProcessCallSiteContextSensitive(call, context, depth, maxDepth, visitor);
    }
}

// 主函数：现在非常简洁
void CPGContext::TraverseCallGraphContextSensitive(
    const clang::FunctionDecl* entry,
    CallGraphVisitor visitor,
    int maxDepth) const
{
    CallContext initialContext;
    TraverseCallGraphDFS(entry, initialContext, 0, maxDepth, visitor);
}

} // namespace cpg