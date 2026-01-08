/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "CPGAnnotation.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>

namespace cpg {

// ============================================
// 构建接口实现
// ============================================

void CPGContext::BuildCPG(const clang::FunctionDecl* func)
{
    if (!func || !func->hasBody()) {
        return;
    }

    llvm::outs() << "Building CPG for function: " << func->getNameAsString() << "\n";

    BuildICFG(func);
    ComputeReachingDefinitions(func);
    BuildPDG(func);

    llvm::outs() << "CPG construction completed for: "
                 << func->getNameAsString() << "\n";
}

void CPGContext::BuildICFGForTranslationUnit()
{
    llvm::outs() << "Building global ICFG...\n";
    clang::SourceManager& sm = astContext.getSourceManager();

    for (clang::Decl* decl : astContext.getTranslationUnitDecl()->decls()) {
        // 跳过系统头文件中的声明
        if (decl->getLocation().isValid() &&
            sm.isInSystemHeader(decl->getLocation())) {
            continue;
        }

        // 获取要处理的函数声明
        const clang::FunctionDecl* func = nullptr;

        // 普通函数
        if (clang::FunctionDecl* funcDecl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            func = funcDecl;
        } else if (auto* funcTemplate = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) { // 模板函数 - 获取模板的定义
            func = funcTemplate->getTemplatedDecl();
        }

        if (func && func->hasBody() && func->isThisDeclarationADefinition()) {
            // 再次检查函数体位置
            if (func->getBody()->getBeginLoc().isValid() &&
                sm.isInSystemHeader(func->getBody()->getBeginLoc())) {
                continue;
            }
            BuildICFG(func);
        }
    }

    BuildCallGraph();
    LinkCallSites();

    llvm::outs() << "Global ICFG construction completed\n";
}

// ============================================
// ICFG构建实现
// ============================================

void CPGContext::BuildICFG(const clang::FunctionDecl* func)
{
    // 【修复】使用 getCanonicalDecl() 规范化函数指针
    const auto* canonicalFunc = func->getCanonicalDecl();

    // 避免重复构建
    if (funcEntries.find(canonicalFunc) != funcEntries.end()) {
        return;
    }

    clang::CFG::BuildOptions options;
    auto cfg = clang::CFG::buildCFG(func, func->getBody(), &astContext, options);
    if (!cfg) {
        llvm::errs() << "Failed to build CFG for: "
                     << func->getNameAsString() << "\n";
        return;
    }

    cfgCache[canonicalFunc] = std::move(cfg);
    const clang::CFG* cfgPtr = cfgCache[canonicalFunc].get();

    ICFGNode* entryNode = CreateICFGNode(ICFGNodeKind::Entry, canonicalFunc);
    ICFGNode* exitNode = CreateICFGNode(ICFGNodeKind::Exit, canonicalFunc);

    funcEntries[canonicalFunc] = entryNode;
    funcExits[canonicalFunc] = exitNode;

    std::map<const clang::CFGBlock*, ICFGNode*> blockFirstNode;
    std::map<const clang::CFGBlock*, ICFGNode*> blockLastNode;

    BuildICFGNodes(canonicalFunc, cfgPtr, blockFirstNode, blockLastNode);
    ConnectICFGBlocks(cfgPtr, blockFirstNode, blockLastNode);
    ConnectICFGEntryExit(canonicalFunc, cfgPtr, entryNode, exitNode,
                         blockFirstNode, blockLastNode);
}

// 辅助函数：处理单个 CFGBlock
void CPGContext::ProcessCFGBlock(
    const clang::CFGBlock* block,
    const clang::FunctionDecl* func,
    std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
    std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode)
{
    ICFGNode* prevNode = nullptr;

    for (const auto& elem : *block) {
        ICFGNode* node = ProcessCFGElement(elem, func, block);
        if (!node) {
            continue;
        }

        if (prevNode) {
            AddICFGEdge(prevNode, node, ICFGEdgeKind::Intraprocedural);
        } else {
            blockFirstNode[block] = node;
        }
        prevNode = node;
    }

    if (prevNode) {
        blockLastNode[block] = prevNode;
    }
}

// 辅助函数：处理单个 CFGElement，返回创建的节点（或 nullptr）
ICFGNode* CPGContext::ProcessCFGElement(
    const clang::CFGElement& elem,
    const clang::FunctionDecl* func,
    const clang::CFGBlock* block)
{
    auto stmt = elem.getAs<clang::CFGStmt>();
    if (!stmt) {
        return nullptr;
    }

    const clang::Stmt* s = stmt->getStmt();
    const clang::CallExpr* call = llvm::dyn_cast<clang::CallExpr>(s);

    ICFGNodeKind nodeKind = call ? ICFGNodeKind::CallSite
                                 : ICFGNodeKind::Statement;

    ICFGNode* node = CreateICFGNode(nodeKind, func);
    node->stmt = s;
    node->cfgBlock = block;
    node->callExpr = call;

    // 【修复】设置被调用函数
    if (call) {
        node->callee = call->getDirectCallee();
    }

    stmtToICFGNode[s] = node;
    return node;
}

void CPGContext::BuildICFGNodes(
    const clang::FunctionDecl* func,
    const clang::CFG* cfg,
    std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
    std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode)
{
    for (const auto* block : *cfg) {
        if (!block) {
            continue;
        }
        ProcessCFGBlock(block, func, blockFirstNode, blockLastNode);
    }
}

    // 辅助函数：判断边类型
ICFGEdgeKind CPGContext::DetermineEdgeKind(
    const clang::CFGBlock* block,
    int succIndex) const
{
    const clang::Stmt* term = block->getTerminatorStmt();
    if (!term) {
        return ICFGEdgeKind::Unconditional;
    }

    bool isConditional = llvm::isa<clang::IfStmt>(term) ||
                         llvm::isa<clang::WhileStmt>(term);
    if (!isConditional) {
        return ICFGEdgeKind::Unconditional;
    }

    return (succIndex == 0) ? ICFGEdgeKind::True : ICFGEdgeKind::False;
}

    // 辅助函数：连接单个 block 的所有后继
void CPGContext::ConnectBlockSuccessors(
    const clang::CFGBlock* block,
    ICFGNode* lastNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode)
{
    int succIndex = 0;
    for (auto it = block->succ_begin(); it != block->succ_end(); ++it) {
        const auto* succBlock = it->getReachableBlock();
        if (!succBlock) {
            continue;
        }

        auto firstIt = blockFirstNode.find(succBlock);
        if (firstIt == blockFirstNode.end()) {
            continue;
        }

        ICFGEdgeKind edgeKind = DetermineEdgeKind(block, succIndex);
        AddICFGEdge(lastNode, firstIt->second, edgeKind);
        succIndex++;
    }
}

    // 主函数：现在非常简洁
void CPGContext::ConnectICFGBlocks(
    const clang::CFG* cfg,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode)
{
    for (const auto* block : *cfg) {
        if (!block) {
            continue;
        }

        auto lastIt = blockLastNode.find(block);
        if (lastIt == blockLastNode.end()) {
            continue;
        }

        ConnectBlockSuccessors(block, lastIt->second, blockFirstNode);
    }
}

// 辅助函数：连接函数入口到第一个 block
// 注意：CFG的Entry block本身是空的，需要找它的后继
void CPGContext::ConnectEntryNode(
    const clang::CFG* cfg,
    ICFGNode* entryNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode)
{
    const clang::CFGBlock* entryBlock = &cfg->getEntry();

    // Entry block 本身是空的，遍历它的后继找到第一个有语句的block
    for (auto it = entryBlock->succ_begin(); it != entryBlock->succ_end(); ++it) {
        const auto* succBlock = it->getReachableBlock();
        if (!succBlock) {
            continue;
        }

        auto firstIt = blockFirstNode.find(succBlock);
        if (firstIt != blockFirstNode.end()) {
            AddICFGEdge(entryNode, firstIt->second, ICFGEdgeKind::Intraprocedural);
        }
    }
}

    // 辅助函数：连接所有出口前驱到出口节点
void CPGContext::ConnectExitNode(
    const clang::CFG* cfg,
    ICFGNode* exitNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode)
{
    const auto* exitBlock = &cfg->getExit();

    for (auto it = exitBlock->pred_begin(); it != exitBlock->pred_end(); ++it) {
        const auto* predBlock = it->getReachableBlock();
        if (!predBlock) {
            continue;
        }

        auto lastIt = blockLastNode.find(predBlock);
        if (lastIt == blockLastNode.end()) {
            continue;
        }

        AddICFGEdge(lastIt->second, exitNode, ICFGEdgeKind::Intraprocedural);
    }
}

void CPGContext::ConnectICFGEntryExit(
    const clang::FunctionDecl* func,
    const clang::CFG* cfg,
    ICFGNode* entryNode,
    ICFGNode* exitNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
    const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode)
{
    ConnectEntryNode(cfg, entryNode, blockFirstNode);
    ConnectExitNode(cfg, exitNode, blockLastNode);
}

// ============================================
// 调用图构建
// ============================================

    // 辅助函数：根据 CallExpr 找到它所属的函数
const clang::FunctionDecl* CPGContext::FindContainingFunctionForCall(
    const clang::CallExpr* call) const
{
    for (const auto& [func, nodes] : icfgNodes) {
        if (ContainsCallExpr(nodes, call)) {
            return func;
        }
    }
    return nullptr;
}

// 辅助函数：检查节点列表中是否包含指定的 CallExpr
bool CPGContext::ContainsCallExpr(
    const std::vector<std::unique_ptr<ICFGNode>>& nodes,
    const clang::CallExpr* call) const
{
    for (const auto& node : nodes) {
        if (node->callExpr == call) {
            return true;
        }
    }
    return false;
}

    // 辅助函数：注册单个调用点
void CPGContext::RegisterCallSite(clang::CallExpr* call)
{
    clang::FunctionDecl* callee = call->getDirectCallee();
    if (!callee) {
        return;
    }

    // 【修复】获取有 body 的定义，然后规范化
    // 确保与 BuildICFG 中使用的指针一致
    const clang::FunctionDecl* calleeToStore = callee;
    if (!callee->hasBody() && callee->getDefinition()) {
        calleeToStore = callee->getDefinition();
    }
    callTargets[call] = calleeToStore->getCanonicalDecl();

    const clang::FunctionDecl* containingFunc = FindContainingFunctionForCall(call);
    if (containingFunc) {
        callSites[containingFunc].insert(call);
    }
}

void CPGContext::BuildCallGraph()
{
    CallGraphBuilder builder(*this);
    builder.SetSourceManager(&astContext.getSourceManager());
    builder.TraverseDecl(astContext.getTranslationUnitDecl());
}

void CPGContext::LinkCallSites()
{
    for (const auto& [caller, calls] : callSites) {
        for (const clang::CallExpr* callExpr : calls) {
            LinkSingleCallSite(caller, callExpr);
        }
    }
}

void CPGContext::LinkSingleCallSite(const clang::FunctionDecl* caller,
    const clang::CallExpr* callExpr)
{
    llvm::outs() << "  Caller: " << caller->getNameAsString() << "\n";

    // 打印 CallExpr 源码
    std::string callSource = GetStmtSource(callExpr);

    ICFGNode* callNode = stmtToICFGNode[callExpr];
    if (!callNode) {
        return;
    }

    const clang::FunctionDecl* callee = callTargets[callExpr];
    if (!callee) {
        llvm::outs() << "  ERROR: callee not found in callTargets\n";
        return;
    }

    // 【修复】获取有 body 的定义
    const clang::FunctionDecl* calleeWithBody = nullptr;
    if (callee->hasBody()) {
        calleeWithBody = callee;
    } else if (callee->getDefinition()) {
        calleeWithBody = callee->getDefinition();
    }

    if (!calleeWithBody) {
        return;
    }

    // 【关键】使用规范化指针查找，与 BuildICFG 中存储的 key 一致
    const auto* canonicalCallee = calleeWithBody->getCanonicalDecl();

    // 打印 funcEntries 中的所有 key
    llvm::outs() << "  funcEntries keys:\n";
    for (const auto& [func, entry] : funcEntries) {
        llvm::outs() << "    - " << func->getNameAsString()
                     << " (ptr: " << func << ")\n";
    }

    ICFGNode* returnNode = CreateICFGNode(ICFGNodeKind::ReturnSite, caller);
    returnNode->callExpr = callExpr;
    returnNode->callee = calleeWithBody;

    ICFGNode* calleeEntry = GetFunctionEntry(canonicalCallee);
    if (calleeEntry) {
        AddICFGEdge(callNode, calleeEntry, ICFGEdgeKind::Call);
    }

    ICFGNode* calleeExit = GetFunctionExit(canonicalCallee);
    if (calleeExit) {
        AddICFGEdge(calleeExit, returnNode, ICFGEdgeKind::Return);
    }

    CreateParameterNodes(caller, canonicalCallee, callExpr, callNode);
}

// 辅助函数：查找已存在的FormalIn节点
ICFGNode* CPGContext::FindFormalInNode(const clang::FunctionDecl* callee, int paramIndex) const
{
    auto* canonicalCallee = callee->getCanonicalDecl();
    auto it = icfgNodes.find(canonicalCallee);
    if (it == icfgNodes.end()) {
        return nullptr;
    }

    for (const auto& node : it->second) {
        if (node->kind == ICFGNodeKind::FormalIn && node->paramIndex == paramIndex) {
            return node.get();
        }
    }
    return nullptr;
}

// 辅助函数：获取实参表达式的变量名
std::string CPGContext::GetArgumentName(const clang::Expr* arg) const
{
    if (!arg) return "";

    // 跳过隐式转换
    arg = arg->IgnoreParenImpCasts();
    if (auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(arg)) { // 如果是 DeclRefExpr，获取变量名
        return declRef->getDecl()->getNameAsString();
    }

    // 否则返回源代码片段
    return GetStmtSource(arg);
}

void CPGContext::CreateParameterNodes(const clang::FunctionDecl* caller,
    const clang::FunctionDecl* callee,
    const clang::CallExpr* callExpr,
    ICFGNode* callNode)
{
    unsigned numArgs = callExpr->getNumArgs();
    unsigned numParams = callee->getNumParams();

    // 取较小值，避免越界
    unsigned numToProcess = std::min(numArgs, numParams);

    for (unsigned i = 0; i < numToProcess; ++i) {
        // 获取实参和形参名
        std::string actualName = GetArgumentName(callExpr->getArg(i));
        std::string formalName = callee->getParamDecl(i)->getNameAsString();

        // 创建 ActualIn 节点（每次调用都创建新的）
        ICFGNode* actualIn = CreateICFGNode(ICFGNodeKind::ActualIn, caller);
        actualIn->paramIndex = i;
        actualIn->callExpr = callExpr;
        actualIn->paramName = actualName;
        actualIn->callee = callee;

        // 查找或创建 FormalIn 节点（每个函数每个参数只创建一次）
        ICFGNode* formalIn = FindFormalInNode(callee, i);
        if (!formalIn) {
            formalIn = CreateICFGNode(ICFGNodeKind::FormalIn, callee);
            formalIn->paramIndex = i;
            formalIn->paramName = formalName;
        }

        // 建立边
        AddICFGEdge(callNode, actualIn, ICFGEdgeKind::ParamIn);
        AddICFGEdge(actualIn, formalIn, ICFGEdgeKind::ParamIn);
    }
}

ICFGNode* CPGContext::CreateICFGNode(ICFGNodeKind kind,
    const clang::FunctionDecl* func)
{
    auto node = std::make_unique<ICFGNode>(kind);
    node->func = func;
    ICFGNode*  nodePtr = node.get();
    // 【关键】使用规范化指针存储，确保跨函数查找一致
    icfgNodes[func->getCanonicalDecl()].push_back(std::move(node));
    return nodePtr;
}

void CPGContext::AddICFGEdge(ICFGNode* from, ICFGNode* to, ICFGEdgeKind kind)
{
    from->successors.push_back({to, kind});
    to->predecessors.push_back({from, kind});
}

// ============================================
// PDG构建实现
// ============================================

void CPGContext::BuildPDG(const clang::FunctionDecl* func)
{
    ComputeDataDependencies(func);
    ComputeControlDependencies(func);
}

void CPGContext::ComputeReachingDefinitions(const clang::FunctionDecl* func)
{
    const clang::CFG* cfg = GetCFG(func);
    if (!cfg) {
        return;
    }

    ReachingDefsInfo& info = reachingDefsMap[func];

    CollectDefsAndUses(cfg, info);
    IterateReachingDefs(cfg, info);
}

void CPGContext::CollectDefsAndUses(const clang::CFG* cfg,
    ReachingDefsInfo& info)
{
    for (const auto* block : *cfg) {
        if (!block) {
            continue;
        }

        for (const clang::CFGElement& elem : *block) {
            if (auto stmt = elem.getAs<clang::CFGStmt>()) {
                const clang::Stmt* s = stmt->getStmt();
                info.definitions[s] = GetDefinedVars(s);
                info.uses[s] = GetUsedVars(s);
            }
        }
    }
}

// 辅助函数：计算 block 的 IN 集合（合并所有前驱的 OUT）
CPGContext::DefsMap CPGContext::ComputeBlockIn(
    const clang::CFGBlock* block,
    const BlockDefsMap& blockOut) const
{
    DefsMap blockIn;

    for (auto it = block->pred_begin(); it != block->pred_end(); ++it) {
        const auto* predBlock = it->getReachableBlock();
        if (!predBlock) {
            continue;
        }

        auto predOutIt = blockOut.find(predBlock);
        if (predOutIt == blockOut.end()) {
            continue;
        }

        for (const auto& [var, defs] : predOutIt->second) {
            blockIn[var].insert(defs.begin(), defs.end());
        }
    }

    return blockIn;
}

// 辅助函数：处理 block 中的语句，应用 kill-gen
void CPGContext::ApplyKillGen(
    const clang::CFGBlock* block,
    DefsMap& currentDefs,
    ReachingDefsInfo& info)
{
    for (const clang::CFGElement& elem : *block) {
        auto stmt = elem.getAs<clang::CFGStmt>();
        if (!stmt) {
            continue;
        }

        const clang::Stmt* s = stmt->getStmt();
        info.reachingDefs[s] = currentDefs;

        for (const auto& def : info.definitions[s]) {
            currentDefs[def].clear();
            currentDefs[def].insert(s);
        }
    }
}

// 辅助函数：处理单个 block，返回是否有变化
bool CPGContext::ProcessBlockReachingDefs(
    const clang::CFGBlock* block,
    BlockDefsMap& blockOut,
    ReachingDefsInfo& info)
{
    DefsMap blockIn = ComputeBlockIn(block, blockOut);
    DefsMap oldOut = blockOut[block];

    blockOut[block] = blockIn;
    ApplyKillGen(block, blockOut[block], info);

    return blockOut[block] != oldOut;
}

void CPGContext::IterateReachingDefs(const clang::CFG* cfg,
    ReachingDefsInfo& info)
{
    BlockDefsMap blockOut;
    bool changed = true;
    int iterations = 0;
    const int kMaxIterations = 100;
    clang::PostOrderCFGView rpo(cfg);

    while (changed && iterations < kMaxIterations) {
        changed = false;
        iterations++;

        // 前向分析使用逆后续遍历 加快收敛
        for (const clang::CFGBlock* block : rpo) {
            if (!block) {
                continue;
            }

            if (ProcessBlockReachingDefs(block, blockOut, info)) {
                changed = true;
            }
        }
    }
}

void CPGContext::ComputeDataDependencies(const clang::FunctionDecl* func)
{
    auto it = reachingDefsMap.find(func);
    if (it == reachingDefsMap.end()) {
        return;
    }

    const auto& reachInfo = it->second;

    for (const auto& [stmt, usedVars] : reachInfo.uses) {
        if (pdgNodes.find(stmt) == pdgNodes.end()) {
            pdgNodes[stmt] = std::make_unique<PDGNode>(stmt, func);
        }

        PDGNode* pdgNode = pdgNodes[stmt].get();

        for (const auto& var : usedVars) {
            std::set<const clang::Stmt*> defs = GetDefinitions(stmt, var);

            for (auto* defStmt : defs) {
                DataDependency dep(defStmt, stmt, var,
                                   DataDependency::DepKind::Flow);
                pdgNode->AddDataDep(dep);
            }
        }
    }
}

void CPGContext::ComputeControlDependencies(const clang::FunctionDecl* func)
{
    std::map<const clang::CFGBlock*,
             std::set<const clang::CFGBlock*>> postDom;
    ComputePostDominators(func, postDom);

    const clang::CFG* cfg = GetCFG(func);
    if (!cfg) {
        return;
    }

    for (const auto* block : *cfg) {
        if (!block) {
            continue;
        }

        auto* term = block->getTerminatorStmt();
        if (!term) {
            continue;
        }

        if (!llvm::isa<clang::IfStmt>(term) &&
            !llvm::isa<clang::WhileStmt>(term)) {
            continue;
        }

        int branchIdx = 0;
        for (auto it = block->succ_begin(); it != block->succ_end();
             ++it, ++branchIdx) {
            const auto* succBlock = it->getReachableBlock();
            if (!succBlock) {
                continue;
            }

            bool branchValue = (branchIdx == 0);
            ProcessControlBranch(block, term, succBlock, branchValue, postDom);
        }
    }
}

// 辅助函数：检查 block 是否被控制 block 后支配
bool CPGContext::IsPostDominatedBy(
    const clang::CFGBlock* current,
    const clang::CFGBlock* block,
    const PostDomMap& postDom) const
{
    auto it = postDom.find(current);
    return it != postDom.end() && it->second.count(block);
}

// 辅助函数：为 block 中的语句添加控制依赖
void CPGContext::AddControlDepsForBlock(
    const clang::CFGBlock* current,
    const clang::Stmt* term,
    bool branchValue,
    const clang::FunctionDecl* func)
{
    for (const clang::CFGElement& elem : *current) {
        auto stmt = elem.getAs<clang::CFGStmt>();
        if (!stmt) {
            continue;
        }

        const clang::Stmt* s = stmt->getStmt();
        EnsurePDGNode(s, func);

        ControlDependency dep(term, s, branchValue);
        pdgNodes[s]->AddControlDep(dep);
    }
}

// 辅助函数：确保 PDG 节点存在
void CPGContext::EnsurePDGNode(const clang::Stmt* s,
    const clang::FunctionDecl* func)
{
    if (pdgNodes.find(s) == pdgNodes.end()) {
        pdgNodes[s] = std::make_unique<PDGNode>(s, func);
    }
}

// 辅助函数：将未访问的后继加入 worklist
void CPGContext::EnqueueSuccessors(
    const clang::CFGBlock* current,
    std::queue<const clang::CFGBlock*>& worklist,
    std::set<const clang::CFGBlock*>& visited)
{
    for (auto it = current->succ_begin(); it != current->succ_end(); ++it) {
        const auto* nextBlock = it->getReachableBlock();
        if (!nextBlock || visited.count(nextBlock)) {
            continue;
        }

        worklist.push(nextBlock);
        visited.insert(nextBlock);
    }
}

void CPGContext::ProcessControlBranch(
    const clang::CFGBlock* block,
    const clang::Stmt* term,
    const clang::CFGBlock* succBlock,
    bool branchValue,
    const PostDomMap& postDom)
{
    std::set<const clang::CFGBlock*> visited;
    std::queue<const clang::CFGBlock*> worklist;

    worklist.push(succBlock);
    visited.insert(succBlock);

    const clang::FunctionDecl* func = GetContainingFunction(term);

    while (!worklist.empty()) {
        const auto* current = worklist.front();
        worklist.pop();

        if (IsPostDominatedBy(current, block, postDom)) {
            continue;
        }

        AddControlDepsForBlock(current, term, branchValue, func);
        EnqueueSuccessors(current, worklist, visited);
    }
}

void CPGContext::ComputePostDominators(
    const clang::FunctionDecl* func,
    std::map<const clang::CFGBlock*,
    std::set<const clang::CFGBlock*>>& postDom)
{
    const clang::CFG* cfg = GetCFG(func);
    if (!cfg) {
        return;
    }

    std::set<const clang::CFGBlock*> allBlocks;
    for (const auto* block : *cfg) {
        if (block) {
            allBlocks.insert(block);
        }
    }

    const auto* exitBlock = &cfg->getExit();
    postDom[exitBlock] = {exitBlock};

    for (const auto* block : *cfg) {
        if (block && block != exitBlock) {
            postDom[block] = allBlocks;
        }
    }

    IteratePostDominators(cfg, exitBlock, postDom);
}

// 辅助函数：计算两个集合的交集并加入 block 自身
CPGContext::BlockSet CPGContext::IntersectWithBlock(
    const BlockSet& set1,
    const BlockSet& set2,
    const clang::CFGBlock* block) const
{
    BlockSet intersection;
    std::set_intersection(
        set1.begin(), set1.end(),
        set2.begin(), set2.end(),
        std::inserter(intersection, intersection.begin())
    );
    intersection.insert(block);
    return intersection;
}

// 辅助函数：计算 block 的新后支配集合
CPGContext::BlockSet CPGContext::ComputeNewPostDom(
    const clang::CFGBlock* block,
    const PostDomMap& postDom) const
{
    BlockSet newPostDom = {block};
    bool firstSucc = true;

    for (auto it = block->succ_begin(); it != block->succ_end(); ++it) {
        const clang::CFGBlock* succBlock = it->getReachableBlock();
        if (!succBlock) {
            continue;
        }

        auto succDomIt = postDom.find(succBlock);
        if (succDomIt == postDom.end()) {
            continue;
        }

        if (firstSucc) {
            newPostDom.insert(succDomIt->second.begin(),
                              succDomIt->second.end());
            firstSucc = false;
        } else {
            newPostDom = IntersectWithBlock(newPostDom, succDomIt->second, block);
        }
    }

    return newPostDom;
}

// 辅助函数：处理单个 block，返回是否有变化
bool CPGContext::UpdateBlockPostDom(
    const clang::CFGBlock* block,
    const clang::CFGBlock* exitBlock,
    PostDomMap& postDom)
{
    if (!block || block == exitBlock) {
        return false;
    }

    BlockSet newPostDom = ComputeNewPostDom(block, postDom);
    if (newPostDom == postDom[block]) {
        return false;
    }

    postDom[block] = std::move(newPostDom);
    return true;
}

void CPGContext::IteratePostDominators(
    const clang::CFG* cfg,
    const clang::CFGBlock* exitBlock,
    PostDomMap& postDom)
{
    bool changed = true;
    int iterations = 0;
    const int kMaxIterations = 100;

    while (changed && iterations < kMaxIterations) {
        changed = false;
        iterations++;

        // 后向分析采用逆序遍历 加快收敛
        for (const clang::CFGBlock *block : *cfg) {
            if (UpdateBlockPostDom(block, exitBlock, postDom)) {
                changed = true;
            }
        }
    }
}

// ============================================
// CPGBuilder类实现
// ============================================

void CPGBuilder::BuildForTranslationUnit(clang::ASTContext& astCtx,
    CPGContext& cpgCtx)
{
    cpgCtx.BuildICFGForTranslationUnit();
    clang::SourceManager& sm = astCtx.getSourceManager();

    for (clang::Decl* decl : astCtx.getTranslationUnitDecl()->decls()) {
        // 跳过系统头文件中的声明
        if (decl->getLocation().isValid() &&
            sm.isInSystemHeader(decl->getLocation())) {
            continue;
        }

        // 获取要处理的函数声明
        const clang::FunctionDecl* func = nullptr;

        // 普通函数
        if (clang::FunctionDecl* funcDecl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            func = funcDecl;
        } else if (clang::FunctionTemplateDecl* funcTemplate =
            llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) { // 模板函数 - 获取模板的定义
            func = funcTemplate->getTemplatedDecl();
        }

        if (func && func->hasBody() && func->isThisDeclarationADefinition()) {
            // 再次检查函数体位置
            if (func->getBody()->getBeginLoc().isValid() &&
                sm.isInSystemHeader(func->getBody()->getBeginLoc())) {
                continue;
            }
            cpgCtx.ComputeReachingDefinitions(func);
            cpgCtx.BuildPDG(func);
        }
    }
}

void CPGBuilder::BuildForFunction(const clang::FunctionDecl* func,
    CPGContext& cpgCtx)
{
    cpgCtx.BuildCPG(func);
}

} // namespace cpg
