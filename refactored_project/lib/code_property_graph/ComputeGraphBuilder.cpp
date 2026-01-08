/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphBuilder.cpp - 从CPG构建计算图（增强版）
 *
 * 增强功能：
 * 1. 跨函数分析 - 深入被调用函数内部
 * 2. 跨函数数据流追踪 - 追踪到调用者的实参定义
 * 3. 锚点去重 - 避免重复构建相同的计算图
 */
#include "ComputeGraph.h"
#include "code_property_graph/CPGAnnotation.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>
#include <stack>
#include <algorithm>
#include <climits>

namespace compute_graph {

ComputeGraphBuilder::ComputeGraphBuilder(cpg::CPGContext& cpgCtx,
                                         clang::ASTContext& astCtx)
    : cpgContext(cpgCtx), astContext(astCtx)
{}

/*
 * EnsurePrecedingStatementsBuilt 完整实现
 * 文件: ComputeGraphBuilder.cpp
 * 位置: 在BuildFromAnchor之后添加（建议在第606行之后）
 *
 * 功能: 确保目标语句之前的所有语句都已按顺序构建
 * 解决: 同一行多个语句（如 len++; if(...) ）的处理顺序问题
 */


void ComputeGraphBuilder::EnsurePrecedingStatementsBuilt(const clang::Stmt* targetStmt)
{
    if (!targetStmt) return;

    const auto& SM = astContext.getSourceManager();

    // ================================================================
    // 步骤1: 向上查找包含目标语句的CompoundStmt
    // ================================================================
    const clang::CompoundStmt* containingCompound = nullptr;
    const clang::Stmt* directChild = targetStmt;  // 记录在compound中的直接子语句

    // 获取父节点
    auto parents = astContext.getParents(*targetStmt);

    // 向上遍历，查找CompoundStmt
    while (!parents.empty()) {
        const auto& parent = parents[0];

        // 找到CompoundStmt，停止查找
        if (auto* compound = parent.get<clang::CompoundStmt>()) {
            containingCompound = compound;
            break;
        }

        // 遇到函数边界，停止查找
        if (parent.get<clang::FunctionDecl>()) {
            break;
        }

        // 继续向上查找
        if (auto* pStmt = parent.get<clang::Stmt>()) {
            directChild = pStmt;
            parents = astContext.getParents(*pStmt);
        } else {
            break;
        }
    }

    // 如果没找到CompoundStmt，说明是函数体的直接子语句，无需预处理
    if (!containingCompound) {
        #ifdef DEBUG_STMT_ORDER
        llvm::outs() << "[EnsurePrecedingStatementsBuilt] No compound found, skipping\n";
        #endif
        return;
    }

    // ================================================================
    // 步骤2: 收集CompoundStmt中的所有语句，并按位置排序
    // ================================================================
    struct StmtLocation {
        const clang::Stmt* stmt;
        unsigned line;
        unsigned col;

        // 排序规则：先按行号，再按列号
        bool operator<(const StmtLocation& other) const {
            if (line != other.line) return line < other.line;
            return col < other.col;
        }
    };

    std::vector<StmtLocation> sortedStmts;

    // 收集所有语句的位置信息
    for (auto* stmt : containingCompound->body()) {
        clang::SourceLocation loc = stmt->getBeginLoc();
        unsigned line = SM.getSpellingLineNumber(loc);
        unsigned col = SM.getSpellingColumnNumber(loc);

        sortedStmts.push_back({stmt, line, col});
    }

    // 按位置排序（行号优先，然后列号）
    std::sort(sortedStmts.begin(), sortedStmts.end());

    // ================================================================
    // 步骤3: 按顺序构建目标语句之前的所有语句
    // ================================================================
    clang::SourceLocation targetLoc = directChild->getBeginLoc();
    unsigned targetLine = SM.getSpellingLineNumber(targetLoc);
    unsigned targetCol = SM.getSpellingColumnNumber(targetLoc);

    int builtCount = 0;
    int skippedCount = 0;

    for (const auto& loc : sortedStmts) {
        // 到达目标语句，停止构建
        if (loc.stmt == directChild) {
            break;
        }

        // 判断是否在目标之前
        // 注意：同一行的前面语句也需要处理
        bool isBefore = (loc.line < targetLine) ||
                       (loc.line == targetLine && loc.col < targetCol);

        if (isBefore) {
            // 检查是否已经构建过
            if (processedStmts.find(loc.stmt) == processedStmts.end()) {
                // 构建语句（深度为0，作为新的起点）
                BuildExpressionTree(loc.stmt, 0);
                builtCount++;
            } else {
                skippedCount++;
            }
        }
    }

}

    // ============================================
// 【新增】从CPG的ICFG添加CFG（控制流）边
// ============================================
void ComputeGraphBuilder::AddCFGEdges()
{
    if (!currentGraph) return;

    int addedCount = 0;
    int skippedCount = 0;

    llvm::outs() << "[AddCFGEdges] Adding CFG edges to compute graph...\n";

    // 遍历计算图中的所有节点
    for (const auto& [nodeId, node] : currentGraph->GetNodes()) {
        // 只处理有对应AST语句的节点
        if (!node->astStmt) {
            continue;
        }

        // 从CPG获取ICFG节点
        cpg::ICFGNode* icfgNode = cpgContext.GetICFGNode(node->astStmt);
        if (!icfgNode) {
            #ifdef DEBUG_CFG_EDGES
            llvm::outs() << "  [" << nodeId << "] No ICFG node found\n";
            #endif
            continue;
        }

        // 遍历ICFG节点的所有后继
        for (const auto& [succICFG, edgeKind] : icfgNode->successors) {
            if (!succICFG || !succICFG->stmt) {
                continue;
            }

            // 检查后继节点是否也在计算图中
            auto it = processedStmts.find(succICFG->stmt);
            if (it == processedStmts.end()) {
                skippedCount++;
                #ifdef DEBUG_CFG_EDGES
                llvm::outs() << "  [" << nodeId << "] -> successor not in graph\n";
                #endif
                continue;  // 后继不在计算图中，跳过
            }

            ComputeNode::NodeId succNodeId = it->second;

            // 根据ICFG边类型选择标签
            std::string label = GetCFGEdgeLabel(edgeKind);

            // 添加Control边（ConnectNodes会自动去重）
            ConnectNodes(nodeId, succNodeId, ComputeEdgeKind::Control, label);
            addedCount++;

            #ifdef DEBUG_CFG_EDGES
            llvm::outs() << "  CFG edge: [" << nodeId << "] -> ["
                         << succNodeId << "] (" << label << ")\n";
            #endif
        }
    }

    llvm::outs() << "  Added " << addedCount << " CFG edges";
    if (skippedCount > 0) {
        llvm::outs() << " (skipped " << skippedCount << " edges to nodes outside graph)";
    }
    llvm::outs() << "\n";
}

// ============================================
// 【新增】辅助函数：将ICFG边类型转换为标签
// ============================================
std::string ComputeGraphBuilder::GetCFGEdgeLabel(cpg::ICFGEdgeKind kind)
{
    switch (kind) {
        case cpg::ICFGEdgeKind::Intraprocedural:
            return "cfg";
        case cpg::ICFGEdgeKind::True:
            return "cfg_true";
        case cpg::ICFGEdgeKind::False:
            return "cfg_false";
        case cpg::ICFGEdgeKind::Unconditional:
            return "cfg";
        case cpg::ICFGEdgeKind::Call:
            return "cfg_call";
        case cpg::ICFGEdgeKind::Return:
            return "cfg_return";
        case cpg::ICFGEdgeKind::ParamIn:
            return "cfg_param_in";
        case cpg::ICFGEdgeKind::ParamOut:
            return "cfg_param_out";
        default:
            return "cfg";
    }
}

std::shared_ptr<ComputeGraph> ComputeGraphBuilder::BuildFromAnchor(
    const AnchorPoint& anchor)
{
    // ================================================================
    // 初始化：清空所有状态
    // ================================================================
    processedStmts.clear();
    forwardTracedStmts.clear();
    processedFunctions.clear();
    currentCallStack.clear();
    currentCallDepth = 0;
    currentLoopInfo = LoopInfo();  // 重置循环信息

    // ================================================================
    // 创建计算图
    // ================================================================
    std::string graphName = anchor.func ? anchor.func->getNameAsString() : "unknown";
    graphName += "_L" + std::to_string(anchor.sourceLine);
    currentGraph = std::make_shared<ComputeGraph>(graphName);

    // ================================================================
    // 设置图属性
    // ================================================================
    currentGraph->SetProperty("anchor_func",
        anchor.func ? anchor.func->getNameAsString() : "unknown");
    currentGraph->SetProperty("anchor_line", std::to_string(anchor.sourceLine));
    currentGraph->SetProperty("anchor_code", anchor.sourceText);
    currentGraph->SetProperty("loop_depth", std::to_string(anchor.loopDepth));

    // 【原有】检查是否是模板函数
    if (anchor.func) {
        bool isTemplate = anchor.func->getDescribedFunctionTemplate() != nullptr ||
                          anchor.func->isFunctionTemplateSpecialization();
        currentGraph->SetProperty("is_template", isTemplate ? "true" : "false");
        if (isTemplate) {
            currentGraph->SetProperty("template_marker", "[TEMPLATE]");
        }
    }

    // ================================================================
    // 【新增】预处理：按顺序构建同一作用域中锚点之前的所有语句
    // 这是解决同行多语句（如 len++; if(ref[len]...) ）问题的关键
    // ================================================================
    llvm::outs() << "\n========================================\n";
    llvm::outs() << "[BuildFromAnchor] Processing anchor at line " << anchor.sourceLine
                 << " in function " << (anchor.func ? anchor.func->getNameAsString() : "unknown") << "\n";
    llvm::outs() << "[BuildFromAnchor] Anchor code: " << anchor.sourceText << "\n";

    // 调用预处理函数：确保前置语句已构建
    EnsurePrecedingStatementsBuilt(anchor.stmt);
    // ================================================================

    // ================================================================
    // 0. 检测包含锚点的循环，创建Loop节点并记录循环变量
    // ================================================================
    if (anchor.loopDepth > 0) {
        currentLoopInfo = BuildContainingLoopNode(anchor.stmt);
    }

    // ================================================================
    // 1. 递归构建锚点表达式树（包括深入函数调用）
    // ================================================================
    auto anchorNodeId = BuildExpressionTree(anchor.stmt, 0);
    auto anchorNode = currentGraph->GetNode(anchorNodeId);
    if (anchorNode) {
        anchorNode->SetProperty("is_anchor", "true");
        anchorNode->loopDepth = anchor.loopDepth;
        anchorNode->containingFunc = anchor.func;
    }

    // 记录锚点ID到循环信息
    currentLoopInfo.anchorNodeId = anchorNodeId;

    // ================================================================
    // 2. 跨函数向后追踪变量定义（循环变量会特殊处理）
    // ================================================================
    TraceAllDefinitionsBackward(anchor.stmt, 0);

    // ================================================================
    // 3. 向前追踪使用点
    // ================================================================
    TraceAllUsesForward(anchor.stmt, 0);

    // ================================================================
    // 4. 对图中所有参数节点追踪到调用点
    // ================================================================
    TraceAllParametersToCallSites();

    // ================================================================
    // 5. 【改进】连接循环结构
    // ================================================================
    if (currentLoopInfo.loopNodeId != 0) {
        // 连接Loop节点到循环体
        ConnectLoopToBody(currentLoopInfo);
        // 将循环体内的循环变量连接到Loop节点
        ConnectLoopVariablesToLoopNode(currentLoopInfo);
        // 【新增】连接外部循环变量初始化到Loop，并清理错误的边
        ConnectLoopVarInitToLoop(currentLoopInfo);
    }

    // ================================================================
    // 【新增】6. 添加CFG边，显示代码执行顺序
    // ================================================================
    AddCFGEdges();
    // ================================================================

    // ================================================================
    // 设置图的得分
    // ================================================================
    currentGraph->SetProperty("score", std::to_string(anchor.score));

    return currentGraph;
}

// 【简化】连接循环节点到锚点
void ComputeGraphBuilder::ConnectLoopToBody(const LoopInfo& loopInfo)
{
    if (loopInfo.loopNodeId == 0 || loopInfo.anchorNodeId == 0) return;

    // 简化：Loop节点直接连接到锚点节点
    ConnectNodes(loopInfo.loopNodeId, loopInfo.anchorNodeId,
                 ComputeEdgeKind::Control, "loop_body");
}

// 【改进】设置循环体内所有节点的循环上下文，并连接循环变量
void ComputeGraphBuilder::ConnectLoopVariablesToLoopNode(const LoopInfo& loopInfo)
{
    if (loopInfo.loopNodeId == 0) return;

    auto loopNode = currentGraph->GetNode(loopInfo.loopNodeId);
    int loopLine = loopNode ? loopNode->sourceLine : 0;

    llvm::outs() << "[ConnectLoopVariablesToLoopNode] Loop node: " << loopInfo.loopNodeId
                 << ", loopLine: " << loopLine
                 << ", bodyStartLine: " << loopInfo.bodyStartLine
                 << ", bodyEndLine: " << loopInfo.bodyEndLine
                 << ", loopVar: " << loopInfo.loopVarName << "\n";

    int markedCount = 0;

    // 遍历所有节点，设置循环上下文
    for (auto& [id, node] : currentGraph->GetNodes()) {
        if (id == loopInfo.loopNodeId) continue;

        // 检查节点是否在循环体内
        bool inLoopBody = false;

        if (loopInfo.bodyStartLine > 0 && loopInfo.bodyEndLine > 0 &&
            node->sourceLine >= loopInfo.bodyStartLine &&
            node->sourceLine <= loopInfo.bodyEndLine) {
            inLoopBody = true;
        }
        // 已经有loopContextId的节点（展开函数中的节点）
        else if (node->loopContextId == loopInfo.loopNodeId) {
            inLoopBody = true;
        }

        if (inLoopBody) {
            // 【关键】设置循环上下文信息
            node->loopContextId = loopInfo.loopNodeId;
            node->loopContextVar = loopInfo.loopVarName;
            node->loopContextLine = loopLine;
            markedCount++;

            // 如果是循环变量节点，连接到Loop节点
            if (!loopInfo.loopVarName.empty() &&
                (node->kind == ComputeNodeKind::Variable ||
                 node->kind == ComputeNodeKind::Parameter) &&
                node->name == loopInfo.loopVarName) {

                // 检查是否已经有从Loop来的边
                bool hasLoopEdge = false;
                for (const auto& edge : currentGraph->GetIncomingEdges(id)) {
                    if (edge->sourceId == loopInfo.loopNodeId) {
                        hasLoopEdge = true;
                        break;
                    }
                }

                if (!hasLoopEdge) {
                    ConnectNodes(loopInfo.loopNodeId, id,
                                 ComputeEdgeKind::DataFlow, loopInfo.loopVarName);
                }
            }
        }
    }

    llvm::outs() << "  Marked " << markedCount << " nodes as in loop\n";
}

// 【新增】连接循环变量的外部初始化到Loop节点，并清理错误的边
void ComputeGraphBuilder::ConnectLoopVarInitToLoop(const LoopInfo& loopInfo)
{
    if (loopInfo.loopNodeId == 0 || loopInfo.loopVarName.empty()) return;

    auto loopNode = currentGraph->GetNode(loopInfo.loopNodeId);
    if (!loopNode) return;

    int loopLine = loopNode->sourceLine;

    // 1. 找到循环变量的外部初始化节点
    // 外部初始化是：行号小于循环行号、名称匹配循环变量、是Variable类型
    // 选择行号最大（最接近循环）的那个
    ComputeNode::NodeId initNodeId = 0;
    int initLine = 0;

    for (const auto& [id, node] : currentGraph->GetNodes()) {
        // 跳过 Loop 节点本身
        if (id == loopInfo.loopNodeId) continue;

        if (node->name == loopInfo.loopVarName &&
            node->kind == ComputeNodeKind::Variable &&
            node->sourceLine > 0 &&
            node->sourceLine < loopLine) {
            // 选择行号最大的（最接近循环的定义）
            if (node->sourceLine > initLine) {
                initLine = node->sourceLine;
                initNodeId = id;
            }
        }
    }

    if (initNodeId == 0) return;  // 没找到外部初始化

    llvm::outs() << "[ConnectLoopVarInitToLoop] Found init node " << initNodeId
                 << " for loop var '" << loopInfo.loopVarName
                 << "' at line " << initLine << "\n";

    // 2. 检查是否已有从initNode到loopNode的边
    bool hasInitEdge = false;
    for (const auto& edge : currentGraph->GetOutgoingEdges(initNodeId)) {
        if (edge->targetId == loopInfo.loopNodeId) {
            hasInitEdge = true;
            break;
        }
    }

    // 3. 创建 initNode -> Loop 的边（如果不存在）
    if (!hasInitEdge) {
        ConnectNodes(initNodeId, loopInfo.loopNodeId,
                     ComputeEdgeKind::DataFlow, "init:" + loopInfo.loopVarName);
        llvm::outs() << "  Created edge: " << initNodeId << " -> "
                     << loopInfo.loopNodeId << " (init)\n";
    }

    // 4. 【关键】移除initNode到其他节点的DataFlow边（只保留到Loop的边）
    // 收集需要移除的边
    std::vector<ComputeEdge::EdgeId> edgesToRemove;

    for (const auto& edge : currentGraph->GetOutgoingEdges(initNodeId)) {
        // 保留到Loop节点的边
        if (edge->targetId == loopInfo.loopNodeId) continue;

        // 移除到其他节点的边（这些是错误追溯产生的）
        if (edge->kind == ComputeEdgeKind::DataFlow) {
            edgesToRemove.push_back(edge->id);
            llvm::outs() << "  Removing edge: " << initNodeId << " -> "
                         << edge->targetId << " (" << edge->label << ")\n";
        }
    }

    // 执行移除
    for (auto edgeId : edgesToRemove) {
        currentGraph->RemoveEdge(edgeId);
    }

    // 5. 标记initNode
    auto initNode = currentGraph->GetNode(initNodeId);
    if (initNode) {
        initNode->SetProperty("is_loop_var_init", "true");
        initNode->SetProperty("loop_node_id", std::to_string(loopInfo.loopNodeId));
    }
}

ComputeGraphBuilder::LoopInfo ComputeGraphBuilder::BuildContainingLoopNode(
    const clang::Stmt* stmt)
{
    LoopInfo info;
    if (!stmt) return info;

    // 向上遍历父节点，找到包含的循环
    auto parents = astContext.getParents(*stmt);
    auto& SM = astContext.getSourceManager();

    // 【新增】首先查找包含的循环语句
    const clang::Stmt* loopStmt = nullptr;

    while (!parents.empty()) {
        const auto& parent = parents[0];

        if (auto* forStmt = parent.get<clang::ForStmt>()) {
            loopStmt = forStmt;
            break;
        }
        else if (auto* whileStmt = parent.get<clang::WhileStmt>()) {
            loopStmt = whileStmt;
            break;
        }
        else if (auto* doStmt = parent.get<clang::DoStmt>()) {
            loopStmt = doStmt;
            break;
        }

        // 继续向上
        if (auto* pStmt = parent.get<clang::Stmt>()) {
            parents = astContext.getParents(*pStmt);
        } else if (auto* pDecl = parent.get<clang::Decl>()) {
            parents = astContext.getParents(*pDecl);
        } else {
            break;
        }
    }

    // 如果没找到循环，直接返回
    if (!loopStmt) return info;

    // =========================================================
    // 【关键修改】检查循环是否已经构建过
    // =========================================================
    auto it = processedStmts.find(loopStmt);
    if (it != processedStmts.end()) {
        // 循环已存在，返回已有的LoopInfo
        info.loopNodeId = it->second;
        info.loopStmt = loopStmt;

        // 【修复】从循环语句重新提取变量名（不依赖loopVar成员）
        if (auto* forStmt = llvm::dyn_cast<clang::ForStmt>(loopStmt)) {
            info.loopVarName = ExtractLoopVarFromFor(forStmt);
        } else if (auto* whileStmt = llvm::dyn_cast<clang::WhileStmt>(loopStmt)) {
            info.loopVarName = ExtractLoopVarFromCondition(whileStmt->getCond());
        } else if (auto* doStmt = llvm::dyn_cast<clang::DoStmt>(loopStmt)) {
            info.loopVarName = ExtractLoopVarFromCondition(doStmt->getCond());
        }

        // 从已有节点恢复行号信息
        auto loopNode = currentGraph->GetNode(info.loopNodeId);
        if (loopNode) {
            info.bodyStartLine = loopNode->sourceLine;
            info.bodyEndLine = info.bodyStartLine + 100;  // 近似值
        }

        return info;  // 直接返回，不重建
    }
    // =========================================================

    // 循环不存在，创建新的循环节点
    // 下面根据循环类型分别处理

    if (auto* forStmt = llvm::dyn_cast<clang::ForStmt>(loopStmt)) {
        // 找到了 for 循环
        info.loopNodeId = BuildExpressionTree(forStmt, 0);
        info.loopStmt = forStmt;

        // 【新增】保存初始化语句信息
        if (auto* init = forStmt->getInit()) {
            info.initStmt = init;
            auto initIt = processedStmts.find(init);
            if (initIt != processedStmts.end()) {
                info.initNodeId = initIt->second;
            }
        }

        // 提取循环变量名
        info.loopVarName = ExtractLoopVarFromFor(forStmt);

        // 【改进】正确计算循环体的行号范围
        if (auto* body = forStmt->getBody()) {
            // 对于 CompoundStmt，使用花括号位置
            if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(body)) {
                // 【关键修复】使用右花括号的行号作为结束行
                info.bodyStartLine = SM.getSpellingLineNumber(compound->getLBracLoc());
                info.bodyEndLine = SM.getSpellingLineNumber(compound->getRBracLoc());

                // 如果花括号位置无效，回退到遍历语句
                if (info.bodyStartLine == 0 || info.bodyEndLine == 0) {
                    if (!compound->body_empty()) {
                        info.bodyStartLine = SM.getSpellingLineNumber(
                            compound->body_front()->getBeginLoc());
                        // 遍历所有语句找最大行号
                        info.bodyEndLine = info.bodyStartLine;
                        for (auto* stmt : compound->body()) {
                            int endLine = SM.getSpellingLineNumber(stmt->getEndLoc());
                            if (endLine > info.bodyEndLine) {
                                info.bodyEndLine = endLine;
                            }
                        }
                    }
                }
            } else {
                info.bodyStartLine = GetSourceLine(body, astContext);
                info.bodyEndLine = GetSourceLine(body, astContext);
            }

            llvm::outs() << "[BuildContainingLoopNode] For loop at line "
                         << GetSourceLine(forStmt, astContext)
                         << ", loopVar: " << info.loopVarName
                         << ", bodyRange: [" << info.bodyStartLine
                         << ", " << info.bodyEndLine << "]\n";
        }
        return info;
    }
    else if (auto* whileStmt = llvm::dyn_cast<clang::WhileStmt>(loopStmt)) {
        info.loopNodeId = BuildExpressionTree(whileStmt, 0);
        info.loopStmt = whileStmt;
        info.loopVarName = ExtractLoopVarFromCondition(whileStmt->getCond());

        if (auto* body = whileStmt->getBody()) {
            if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(body)) {
                info.bodyStartLine = SM.getSpellingLineNumber(compound->getLBracLoc());
                info.bodyEndLine = SM.getSpellingLineNumber(compound->getRBracLoc());
                if (info.bodyStartLine == 0 || info.bodyEndLine == 0) {
                    if (!compound->body_empty()) {
                        info.bodyStartLine = SM.getSpellingLineNumber(
                            compound->body_front()->getBeginLoc());
                        info.bodyEndLine = info.bodyStartLine;
                        for (auto* stmt : compound->body()) {
                            int endLine = SM.getSpellingLineNumber(stmt->getEndLoc());
                            if (endLine > info.bodyEndLine) info.bodyEndLine = endLine;
                        }
                    }
                }
            } else {
                info.bodyStartLine = GetSourceLine(body, astContext);
                info.bodyEndLine = info.bodyStartLine;
            }
        }
        return info;
    }
    else if (auto* doStmt = llvm::dyn_cast<clang::DoStmt>(loopStmt)) {
        info.loopNodeId = BuildExpressionTree(doStmt, 0);
        info.loopStmt = doStmt;
        info.loopVarName = ExtractLoopVarFromCondition(doStmt->getCond());

        if (auto* body = doStmt->getBody()) {
            if (auto* compound = llvm::dyn_cast<clang::CompoundStmt>(body)) {
                info.bodyStartLine = SM.getSpellingLineNumber(compound->getLBracLoc());
                info.bodyEndLine = SM.getSpellingLineNumber(compound->getRBracLoc());
                if (info.bodyStartLine == 0 || info.bodyEndLine == 0) {
                    if (!compound->body_empty()) {
                        info.bodyStartLine = SM.getSpellingLineNumber(
                            compound->body_front()->getBeginLoc());
                        info.bodyEndLine = info.bodyStartLine;
                        for (auto* stmt : compound->body()) {
                            int endLine = SM.getSpellingLineNumber(stmt->getEndLoc());
                            if (endLine > info.bodyEndLine) info.bodyEndLine = endLine;
                        }
                    }
                }
            } else {
                info.bodyStartLine = GetSourceLine(body, astContext);
                info.bodyEndLine = info.bodyStartLine;
            }
        }
        return info;
    }

    return info;
}

// 【保持不变】从for循环中提取循环变量名
std::string ComputeGraphBuilder::ExtractLoopVarFromFor(const clang::ForStmt* forStmt)
{
    if (!forStmt) return "";

    // 优先从增量表达式中提取 (++i, i++, i+=1 等)
    if (auto* inc = forStmt->getInc()) {
        // 处理 ++i 或 i++
        if (auto* unaryOp = llvm::dyn_cast<clang::UnaryOperator>(inc)) {
            if (unaryOp->isIncrementDecrementOp()) {
                if (auto* declRef = llvm::dyn_cast<clang::DeclRefExpr>(
                        unaryOp->getSubExpr()->IgnoreParenImpCasts())) {
                    return declRef->getDecl()->getNameAsString();
                }
            }
        }
        // 处理 i += 1 或 i = i + 1
        else if (auto* binOp = llvm::dyn_cast<clang::BinaryOperator>(inc)) {
            if (binOp->isAssignmentOp() || binOp->isCompoundAssignmentOp()) {
                if (auto* lhs = llvm::dyn_cast<clang::DeclRefExpr>(
                        binOp->getLHS()->IgnoreParenImpCasts())) {
                    return lhs->getDecl()->getNameAsString();
                }
            }
        }
    }

    // 从条件表达式中提取
    return ExtractLoopVarFromCondition(forStmt->getCond());
}


    // 【保持不变】从条件表达式中提取循环变量名
std::string ComputeGraphBuilder::ExtractLoopVarFromCondition(const clang::Expr* cond)
{
    if (!cond) return "";

    // 处理 i < n, i <= n, i != n 等比较表达式
    if (auto* binOp = llvm::dyn_cast<clang::BinaryOperator>(cond->IgnoreParenImpCasts())) {
        if (binOp->isComparisonOp()) {
            // 通常左边是循环变量
            if (auto* lhs = llvm::dyn_cast<clang::DeclRefExpr>(
                    binOp->getLHS()->IgnoreParenImpCasts())) {
                return lhs->getDecl()->getNameAsString();
                    }
            // 也检查右边 (如 n > i)
            if (auto* rhs = llvm::dyn_cast<clang::DeclRefExpr>(
                    binOp->getRHS()->IgnoreParenImpCasts())) {
                return rhs->getDecl()->getNameAsString();
                    }
        }
    }

    return "";
}
// ============================================
// 【新增】分支相关函数实现
// ============================================
void ComputeGraphBuilder::MarkNodesInBranch(const BranchInfo& branchInfo)
{
    if (branchInfo.branchNodeId == 0) return;

    int markedCount = 0;

    // 遍历所有节点，标注在分支内的节点
    for (auto& [id, node] : currentGraph->GetNodes()) {
        if (id == branchInfo.branchNodeId) continue;

        // 检查节点是否在分支范围内
        bool inBranch = false;
        std::string branchLabel;

        if (!branchInfo.branchType.empty()) {
            if (branchInfo.bodyStartLine > 0 && branchInfo.bodyEndLine > 0 &&
                node->sourceLine >= branchInfo.bodyStartLine &&
                node->sourceLine <= branchInfo.bodyEndLine) {

                // 匹配分支类型
                if (branchInfo.branchType == "THEN") {
                    inBranch = true;
                    branchLabel = "THEN";
                } else if (branchInfo.branchType == "ELSE") {
                    inBranch = true;
                    branchLabel = "ELSE";
                } else if (branchInfo.branchType.rfind("CASE", 0) == 0) { // 处理 CASE x
                    inBranch = true;
                    branchLabel = branchInfo.branchType;
                } else if (branchInfo.branchType == "DEFAULT") {
                    inBranch = true;
                    branchLabel = "DEFAULT";
                }
                }
        }

        if (inBranch) {
            // 设置分支上下文属性
            node->branchContextId = branchInfo.branchNodeId;
            node->branchType = branchInfo.branchType;
            node->branchContextLine = branchInfo.branchLine;

            // 【优化】新开一栏属性 branch_label，不再修改 node->name
            // 这样你的可视化工具可以读取这个属性并显示在独立列中
            node->SetProperty("branch_label", branchLabel);

            markedCount++;
        }
    }
}



ComputeNode::NodeId ComputeGraphBuilder::BuildIfBranch(
    const clang::IfStmt* ifStmt, int depth)
{
    if (!ifStmt) return 0;

    // 【修改】使用统一的深度限制（原来是maxExprDepth）
    if (depth >= maxBackwardDepth) {
        return 0;
    }

    // 【新增】去重检查 - 避免重复处理同一个if语句
    auto it = processedStmts.find(ifStmt);
    if (it != processedStmts.end()) {
        return it->second;
    }

    // 1. 创建Branch节点
    auto branchNode = currentGraph->CreateNode(ComputeNodeKind::Branch);
    if (!branchNode) return 0;

    auto branchId = branchNode->id;

    branchNode->kind = ComputeNodeKind::Branch;
    branchNode->name = "if";
    branchNode->astStmt = ifStmt;
    branchNode->sourceLine = GetSourceLine(ifStmt, astContext);
    branchNode->sourceText = "if (" + GetSourceText(ifStmt->getCond(), astContext) + ")";
    branchNode->containingFunc = GetContainingFunction(ifStmt);

    // 继承循环上下文
    if (currentLoopInfo.loopNodeId != 0) {
        branchNode->loopContextId = currentLoopInfo.loopNodeId;
        branchNode->loopContextVar = currentLoopInfo.loopVarName;
        branchNode->loopContextLine = currentLoopInfo.bodyStartLine;
    }

    processedStmts[ifStmt] = branchId;

    // 2. 处理条件表达式
    auto* cond = ifStmt->getCond();
    if (cond) {
        auto condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, branchId, ComputeEdgeKind::Control, "condition");
        }
    }

    // 3. 准备BranchInfo (基础信息)
    BranchInfo branchInfo;
    branchInfo.branchNodeId = branchId;
    branchInfo.branchStmt = ifStmt;
    branchInfo.condition = ifStmt->getCond();
    branchInfo.branchLine = GetSourceLine(ifStmt, astContext);

    // 4. 处理THEN分支
    auto* thenStmt = ifStmt->getThen();
    if (thenStmt) {
        // 为 THEN 分支设置专门的 info
        branchInfo.branchType = "THEN";
        branchInfo.bodyStartLine = GetSourceLine(thenStmt, astContext);

        auto& srcMgr = astContext.getSourceManager();
        auto endLoc = thenStmt->getEndLoc();
        if (endLoc.isValid()) {
            branchInfo.bodyEndLine = srcMgr.getSpellingLineNumber(endLoc);
        } else {
            branchInfo.bodyEndLine = branchInfo.bodyStartLine;
        }

        auto thenId = BuildBranchBody(thenStmt, depth + 1, "THEN", branchInfo);
        if (thenId != 0) {
            ConnectNodes(branchId, thenId, ComputeEdgeKind::Control, "then");
        }

        // 处理完 THEN 后立刻标注，防止 info 被 ELSE 覆盖
        MarkNodesInBranch(branchInfo);
    }

    // 5. 处理ELSE分支
    auto* elseStmt = ifStmt->getElse();
    if (elseStmt) {
        // 为 ELSE 分支更新 info
        branchInfo.branchType = "ELSE";
        branchInfo.bodyStartLine = GetSourceLine(elseStmt, astContext);

        auto& srcMgr = astContext.getSourceManager();
        auto endLoc = elseStmt->getEndLoc();
        if (endLoc.isValid()) {
            branchInfo.bodyEndLine = srcMgr.getSpellingLineNumber(endLoc);
        } else {
            branchInfo.bodyEndLine = branchInfo.bodyStartLine;
        }

        auto elseId = BuildBranchBody(elseStmt, depth + 1, "ELSE", branchInfo);
        if (elseId != 0) {
            ConnectNodes(branchId, elseId, ComputeEdgeKind::Control, "else");
        }

        // 处理完 ELSE 后立刻标注
        MarkNodesInBranch(branchInfo);
    }

    return branchId;
}

    ComputeNode::NodeId ComputeGraphBuilder::BuildBranchBody(
        const clang::Stmt* body, int depth, const std::string& branchType,
        const BranchInfo& parentBranch)
{
    if (!body) return 0;

    // 【修改】使用统一的深度限制（原来是maxExprDepth）
    if (depth >= maxBackwardDepth) {
        return 0;
    }

    // 【新增】快速跳过控制流语句（break/continue/return）
    if (llvm::isa<clang::BreakStmt>(body) ||
        llvm::isa<clang::ContinueStmt>(body) ||
        llvm::isa<clang::ReturnStmt>(body)) {
        return 0;  // 这些语句不需要深入分析
        }

    // 【新增】去重检查 - 避免重复处理同一个语句体
    if (processedStmts.count(body)) {
        return processedStmts[body];
    }

    // 保存当前分支上下文
    BranchInfo savedContext = currentBranchContext;

    // 设置新的分支上下文
    currentBranchContext = parentBranch;
    currentBranchContext.branchType = branchType;

    // 【修改】将日志包装在DEBUG宏中
#ifdef DEBUG_BRANCH_VERBOSE
    llvm::outs() << "  [BuildBranchBody] Building " << branchType
                 << " body (lines " << currentBranchContext.bodyStartLine
                 << "-" << currentBranchContext.bodyEndLine << ")\n";
#endif

    // 构建body
    ComputeNode::NodeId bodyId = 0;

    // 如果body是CompoundStmt，处理其中的每个语句
    if (auto* compoundStmt = llvm::dyn_cast<clang::CompoundStmt>(body)) {
        for (auto* stmt : compoundStmt->body()) {
            // 【新增】跳过已处理的语句
            if (processedStmts.count(stmt)) {
                continue;
            }

            auto stmtId = BuildExpressionTree(stmt, depth);
            if (stmtId != 0 && bodyId == 0) {
                bodyId = stmtId;  // 返回第一个语句节点
            }
        }
    } else {
        // 单个语句
        bodyId = BuildExpressionTree(body, depth);
    }

    // 恢复上下文
    currentBranchContext = savedContext;

    return bodyId;
}

ComputeNode::NodeId ComputeGraphBuilder::CreateNodeFromExpr(const clang::Expr* expr)
{
    return CreateNodeFromStmt(expr);
}

ComputeNode::NodeId ComputeGraphBuilder::CreateNodeFromDecl(const clang::Decl* decl)
{
    if (auto* varDecl = llvm::dyn_cast<clang::VarDecl>(decl)) {
        auto node = currentGraph->CreateNode(ComputeNodeKind::Variable);
        node->name = varDecl->getNameAsString();
        node->dataType = DataTypeInfo::FromClangType(varDecl->getType());
        node->astDecl = decl;
        return node->id;
    }

    if (auto* paramDecl = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
        auto node = currentGraph->CreateNode(ComputeNodeKind::Parameter);
        node->name = paramDecl->getNameAsString();
        node->dataType = DataTypeInfo::FromClangType(paramDecl->getType());
        node->astDecl = decl;
        return node->id;
    }

    return 0;
}

OpCode ComputeGraphBuilder::GetOpCodeFromBinaryOp(
    const clang::BinaryOperator* binOp) const
{
    switch (binOp->getOpcode()) {
        case clang::BO_Add:
        case clang::BO_AddAssign:
            return OpCode::Add;
        case clang::BO_Sub:
        case clang::BO_SubAssign:
            return OpCode::Sub;
        case clang::BO_Mul:
        case clang::BO_MulAssign:
            return OpCode::Mul;
        case clang::BO_Div:
        case clang::BO_DivAssign:
            return OpCode::Div;
        case clang::BO_Rem:
            return OpCode::Mod;
        case clang::BO_And:
            return OpCode::And;
        case clang::BO_Or:
            return OpCode::Or;
        case clang::BO_Xor:
            return OpCode::Xor;
        case clang::BO_Shl:
            return OpCode::Shl;
        case clang::BO_Shr:
            return OpCode::Shr;
        case clang::BO_LT:
            return OpCode::Lt;
        case clang::BO_GT:
            return OpCode::Gt;
        case clang::BO_LE:
            return OpCode::Le;
        case clang::BO_GE:
            return OpCode::Ge;
        case clang::BO_EQ:
            return OpCode::Eq;
        case clang::BO_NE:
            return OpCode::Ne;
        case clang::BO_Assign:
            return OpCode::Assign;
        default:
            return OpCode::Unknown;
    }
}

OpCode ComputeGraphBuilder::GetOpCodeFromUnaryOp(
    const clang::UnaryOperator* unaryOp) const
{
    switch (unaryOp->getOpcode()) {
        case clang::UO_Minus:
            return OpCode::Neg;
        case clang::UO_Not:
            return OpCode::BitNot;
        case clang::UO_LNot:
            return OpCode::Not;
        case clang::UO_PreInc:
        case clang::UO_PostInc:
            return OpCode::Add;
        case clang::UO_PreDec:
        case clang::UO_PostDec:
            return OpCode::Sub;
        default:
            return OpCode::Unknown;
    }
}

std::string ComputeGraphBuilder::GetOperatorName(OpCode opCode) const
{
    return OpCodeToString(opCode);
}

void ComputeGraphBuilder::ConnectNodes(ComputeNode::NodeId from,
                                       ComputeNode::NodeId to,
                                       ComputeEdgeKind kind,
                                       const std::string& label)
{
    if (from == to || from == 0 || to == 0) return;

    auto existingEdges = currentGraph->GetOutgoingEdges(from);
    for (const auto& edge : existingEdges) {
        if (edge->targetId == to && edge->kind == kind &&
            edge->label == label) {
            return;
        }
    }

    currentGraph->AddEdge(from, to, kind, label);
}

void ComputeGraphBuilder::TraceDefinitionsBackward(const clang::Expr* expr, int depth)
{
    TraceAllDefinitionsBackward(expr, depth);
}

void ComputeGraphBuilder::TraceUsesForward(const clang::Stmt* stmt,
                                           const std::string& varName, int depth)
{
    TraceAllUsesForward(stmt, depth);
}

void ComputeGraphBuilder::TraceExprOperands(const clang::Expr* expr,
                                            ComputeNode::NodeId parentId, int depth)
{
    // 由BuildExpressionTree处理
}

// ============================================
// ComputeGraphMerger 实现
// ============================================

std::shared_ptr<ComputeGraph> ComputeGraphMerger::Merge(
    const ComputeGraph& g1, const ComputeGraph& g2)
{
    auto merged = std::make_shared<ComputeGraph>(g1.GetName() + "_merged");

    std::map<ComputeNode::NodeId, ComputeNode::NodeId> g1Map, g2Map;
    std::map<const clang::Stmt*, ComputeNode::NodeId> stmtToNode;

    for (const auto& node : g1.GetAllNodes()) {
        auto newNode = merged->CreateNode(node->kind);
        CopyNodeProperties(node.get(), newNode.get());
        g1Map[node->id] = newNode->id;

        if (node->astStmt) {
            stmtToNode[node->astStmt] = newNode->id;
        }
    }

    for (const auto& edge : g1.GetAllEdges()) {
        merged->AddEdge(g1Map[edge->sourceId], g1Map[edge->targetId],
                        edge->kind, edge->label);
    }

    for (const auto& node : g2.GetAllNodes()) {
        auto stmtIt = stmtToNode.find(node->astStmt);
        if (stmtIt != stmtToNode.end() && node->astStmt) {
            g2Map[node->id] = stmtIt->second;
        } else {
            auto newNode = merged->CreateNode(node->kind);
            CopyNodeProperties(node.get(), newNode.get());
            g2Map[node->id] = newNode->id;

            if (node->astStmt) {
                stmtToNode[node->astStmt] = newNode->id;
            }
        }
    }

    for (const auto& edge : g2.GetAllEdges()) {
        auto fromId = g2Map[edge->sourceId];
        auto toId = g2Map[edge->targetId];

        bool exists = false;
        for (const auto& existingEdge : merged->GetOutgoingEdges(fromId)) {
            if (existingEdge->targetId == toId &&
                existingEdge->kind == edge->kind) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            merged->AddEdge(fromId, toId, edge->kind, edge->label);
        }
    }

    return merged;
}

void ComputeGraphMerger::CopyNodeProperties(const ComputeNode* src, ComputeNode* dst)
{
    dst->name = src->name;
    dst->opCode = src->opCode;
    dst->dataType = src->dataType;
    dst->hasConstValue = src->hasConstValue;
    dst->constValue = src->constValue;
    dst->loopDepth = src->loopDepth;
    dst->astStmt = src->astStmt;
    dst->astDecl = src->astDecl;
    dst->containingFunc = src->containingFunc;
    dst->sourceText = src->sourceText;
    dst->sourceLine = src->sourceLine;
    dst->properties = src->properties;
}

std::shared_ptr<ComputeGraph> ComputeGraphMerger::MergeAll(
    const std::vector<std::shared_ptr<ComputeGraph>>& graphs)
{
    if (graphs.empty()) return nullptr;
    if (graphs.size() == 1) return graphs[0];

    auto result = graphs[0];
    for (size_t i = 1; i < graphs.size(); ++i) {
        result = Merge(*result, *graphs[i]);
    }
    return result;
}

bool ComputeGraphMerger::HasOverlap(const ComputeGraph& g1, const ComputeGraph& g2)
{
    std::set<const clang::Stmt*> g1Stmts;
    for (const auto& node : g1.GetAllNodes()) {
        if (node->astStmt) {
            g1Stmts.insert(node->astStmt);
        }
    }

    for (const auto& node : g2.GetAllNodes()) {
        if (node->astStmt && g1Stmts.count(node->astStmt)) {
            return true;
        }
    }

    return false;
}

// ============================================
// 全局辅助函数
// ============================================

void MergeOverlappingGraphs(ComputeGraphSet& graphSet)
{
    graphSet.MergeOverlapping();
}

    std::shared_ptr<ComputeGraph> ComputeGraphBuilder::BuildFromFunction(
        const clang::FunctionDecl* func)
{
    if (!func || !func->hasBody()) {
        return nullptr;
    }

    processedStmts.clear();
    forwardTracedStmts.clear();
    processedFunctions.clear();
    currentCallDepth = 0;
    currentGraph = std::make_shared<ComputeGraph>(func->getNameAsString());

    for (const clang::ParmVarDecl* param : func->parameters()) {
        std::shared_ptr<ComputeNode> paramNode =
            currentGraph->CreateNode(ComputeNodeKind::Parameter);
        paramNode->name = param->getNameAsString();
        paramNode->dataType = DataTypeInfo::FromClangType(param->getType());
        paramNode->astDecl = param;
        paramNode->containingFunc = func;
    }

    AnchorFinder finder(cpgContext, astContext);
    std::vector<AnchorPoint> anchors = finder.FindAnchorsInFunction(func);
    std::vector<AnchorPoint> rankedAnchors = finder.FilterAndRankAnchors(anchors);

    for (const AnchorPoint& anchor : rankedAnchors) {
        BuildExpressionTree(anchor.stmt, 0);
    }

    for (const AnchorPoint& anchor : rankedAnchors) {
        TraceAllDefinitionsBackward(anchor.stmt, 0);
        TraceAllUsesForward(anchor.stmt, 0);
    }

    return currentGraph;
}

void ComputeGraphBuilder::AnalyzeCalleeBody(
    const clang::FunctionDecl* callee,
    ComputeNode::NodeId callNodeId,
    const clang::CallExpr* callExpr)
{
    if (ShouldSkipCalleeAnalysis(callee)) {
        return;
    }

    std::shared_ptr<ComputeNode> callNode = currentGraph->GetNode(callNodeId);
    if (callNode) {
        callNode->SetProperty("callee_analyzed", "true");
        callNode->SetProperty("callee_name", callee->getNameAsString());
    }

    ComputeNode::NodeId inheritedLoopContextId = 0;
    std::string inheritedLoopContextVar;
    int inheritedLoopContextLine = 0;

    InheritLoopContext(callNodeId, inheritedLoopContextId,
                      inheritedLoopContextVar, inheritedLoopContextLine);

    ClearCalleeStmts(callee);

    std::map<const clang::ParmVarDecl*, ComputeNode::NodeId> paramToNodeId;
    CreateParamNodesForCallee(callee, callExpr, callNodeId,
                             inheritedLoopContextId, inheritedLoopContextVar,
                             inheritedLoopContextLine, paramToNodeId);

    RegisterParamRefsInCallee(callee, paramToNodeId);

    ProcessCalleeBodyStmts(callee, callNodeId, inheritedLoopContextId,
                          inheritedLoopContextVar, inheritedLoopContextLine);

    PropagateContextToCalleeNodes(callee, callNodeId,
                                 inheritedLoopContextId,
                                 inheritedLoopContextVar,
                                 inheritedLoopContextLine);
}

// ============================================
// 辅助函数实现
// ============================================

bool ComputeGraphBuilder::ShouldSkipCalleeAnalysis(
    const clang::FunctionDecl* callee)
{
    if (!callee || !callee->hasBody()) {
        return true;
    }

    const clang::SourceManager& sm = astContext.getSourceManager();
    if (IsVectorIntrinsicFunction(callee, sm)) {
        return true;
    }

    return false;
}

void ComputeGraphBuilder::InheritLoopContext(
    ComputeNode::NodeId callNodeId,
    ComputeNode::NodeId& inheritedLoopContextId,
    std::string& inheritedLoopContextVar,
    int& inheritedLoopContextLine)
{
    std::shared_ptr<ComputeNode> callNode = currentGraph->GetNode(callNodeId);
    if (!callNode) {
        return;
    }

    if (callNode->loopContextId != 0) {
        inheritedLoopContextId = callNode->loopContextId;
        inheritedLoopContextVar = callNode->loopContextVar;
        inheritedLoopContextLine = callNode->loopContextLine;
        return;
    }

    if (currentLoopInfo.loopNodeId == 0) {
        return;
    }

    int callLine = callNode->sourceLine;
    bool inCurrentLoop = (callLine >= currentLoopInfo.bodyStartLine &&
                          callLine <= currentLoopInfo.bodyEndLine);

    if (inCurrentLoop) {
        inheritedLoopContextId = currentLoopInfo.loopNodeId;
        inheritedLoopContextVar = currentLoopInfo.loopVarName;

        std::shared_ptr<ComputeNode> loopNode =
            currentGraph->GetNode(currentLoopInfo.loopNodeId);
        inheritedLoopContextLine = loopNode ? loopNode->sourceLine : 0;
    }
}

void ComputeGraphBuilder::ClearCalleeStmts(const clang::FunctionDecl* callee)
{
    class StmtCollector : public clang::RecursiveASTVisitor<StmtCollector> {
    public:
        std::set<const clang::Stmt*> stmts;

        bool VisitStmt(clang::Stmt* s) {
            stmts.insert(s);
            return true;
        }
    };

    StmtCollector collector;
    collector.TraverseStmt(callee->getBody());

    for (const clang::Stmt* s : collector.stmts) {
        processedStmts.erase(s);
    }
}

    void ComputeGraphBuilder::CreateParamNodesForCallee(
    const clang::FunctionDecl* callee,
    const clang::CallExpr* callExpr,
    ComputeNode::NodeId callNodeId,
    ComputeNode::NodeId inheritedLoopContextId,
    const std::string& inheritedLoopContextVar,
    int inheritedLoopContextLine,
    std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId)
{
    unsigned numParams = callee->getNumParams();
    unsigned numArgs = callExpr->getNumArgs();

    for (unsigned i = 0; i < numParams && i < numArgs; ++i) {
        const clang::ParmVarDecl* param = callee->getParamDecl(i);
        const clang::Expr* arg = callExpr->getArg(i);

        std::shared_ptr<ComputeNode> paramNode =
            currentGraph->CreateNode(ComputeNodeKind::Parameter);
        paramNode->name = param->getNameAsString();
        paramNode->dataType = DataTypeInfo::FromClangType(param->getType());
        paramNode->astDecl = param;
        paramNode->containingFunc = callee;
        paramNode->SetProperty("is_formal_param", "true");
        paramNode->SetProperty("call_site_id", std::to_string(callNodeId));

        if (inheritedLoopContextId != 0) {
            paramNode->loopContextId = inheritedLoopContextId;
            paramNode->loopContextVar = inheritedLoopContextVar;
            paramNode->loopContextLine = inheritedLoopContextLine;
            paramNode->SetProperty("in_loop_context", "true");
        }

        paramToNodeId[param] = paramNode->id;

        ComputeNode::NodeId argId =
            BuildExpressionTree(arg->IgnoreParenImpCasts(), 0);
        if (argId != 0) {
            ConnectNodes(argId, paramNode->id, ComputeEdgeKind::Call,
                        "param_" + std::to_string(i));
        }
    }
}

void ComputeGraphBuilder::RegisterParamRefsInCallee(
    const clang::FunctionDecl* callee,
    const std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId)
{
    class ParamRefRegistrar : public clang::RecursiveASTVisitor<ParamRefRegistrar> {
    public:
        const std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramNodeIds;
        std::map<const clang::Stmt*, ComputeNode::NodeId>& stmtMap;

        ParamRefRegistrar(
            const std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& pids,
            std::map<const clang::Stmt*, ComputeNode::NodeId>& sm)
            : paramNodeIds(pids), stmtMap(sm) {}

        bool VisitDeclRefExpr(clang::DeclRefExpr* ref) {
            const clang::ParmVarDecl* param =
                llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());

            if (param) {
                std::map<const clang::ParmVarDecl*,
                        ComputeNode::NodeId>::const_iterator it =
                    paramNodeIds.find(param);

                if (it != paramNodeIds.end()) {
                    stmtMap[ref] = it->second;
                }
            }
            return true;
        }
    };

    ParamRefRegistrar registrar(paramToNodeId, processedStmts);
    registrar.TraverseStmt(callee->getBody());
}

void ComputeGraphBuilder::ProcessCalleeBodyStmts(
    const clang::FunctionDecl* callee,
    ComputeNode::NodeId callNodeId,
    ComputeNode::NodeId inheritedLoopContextId,
    const std::string& inheritedLoopContextVar,
    int inheritedLoopContextLine)
{
    class BodyCollector : public clang::RecursiveASTVisitor<BodyCollector> {
    public:
        std::vector<const clang::ReturnStmt*> returns;
        std::vector<const clang::BinaryOperator*> assignments;
        std::vector<const clang::DeclStmt*> decls;

        bool VisitReturnStmt(clang::ReturnStmt* ret) {
            returns.push_back(ret);
            return true;
        }

        bool VisitBinaryOperator(clang::BinaryOperator* binOp) {
            if (binOp->isAssignmentOp()) {
                assignments.push_back(binOp);
            }
            return true;
        }

        bool VisitDeclStmt(clang::DeclStmt* decl) {
            decls.push_back(decl);
            return true;
        }
    };

    BodyCollector bodyCollector;
    bodyCollector.TraverseStmt(callee->getBody());

    SetLoopContextFunc setLoopContext = [&](ComputeNode::NodeId nodeId) {
        if (nodeId == 0 || inheritedLoopContextId == 0) {
            return;
        }
        std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
        if (node) {
            node->loopContextId = inheritedLoopContextId;
            node->loopContextVar = inheritedLoopContextVar;
            node->loopContextLine = inheritedLoopContextLine;
            node->SetProperty("in_loop_context", "true");
        }
    };

    for (const clang::DeclStmt* declStmt : bodyCollector.decls) {
        ComputeNode::NodeId nodeId = BuildExpressionTree(declStmt, 0);
        if (nodeId != 0) {
            std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
            if (node) {
                node->containingFunc = callee;
                node->SetProperty("call_site_id", std::to_string(callNodeId));
            }
            setLoopContext(nodeId);
        }
    }

    for (const clang::BinaryOperator* assign : bodyCollector.assignments) {
        ComputeNode::NodeId nodeId = BuildExpressionTree(assign, 0);
        if (nodeId != 0) {
            std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
            if (node) {
                node->containingFunc = callee;
                node->SetProperty("call_site_id", std::to_string(callNodeId));
            }
            setLoopContext(nodeId);
        }
    }

    ProcessReturnStmts(bodyCollector.returns, callNodeId, callee, setLoopContext);
}

typedef std::function<void(ComputeNode::NodeId)> SetLoopContextFunc;

    void ComputeGraphBuilder::ProcessReturnStmts(
    const std::vector<const clang::ReturnStmt*>& returns,
    ComputeNode::NodeId callNodeId,
    const clang::FunctionDecl* callee,
    SetLoopContextFunc setLoopContext)
{
    std::shared_ptr<ComputeNode> callNode = currentGraph->GetNode(callNodeId);
    bool hasExplicitReturn = false;
    std::vector<ComputeNode::NodeId> returnNodeIds;

    for (const clang::ReturnStmt* retStmt : returns) {
        const clang::Expr* retVal = retStmt->getRetValue();
        if (!retVal) {
            continue;
        }

        hasExplicitReturn = true;
        ComputeNode::NodeId retNodeId =
            BuildExpressionTree(retVal->IgnoreParenImpCasts(), 0);

        if (retNodeId != 0) {
            returnNodeIds.push_back(retNodeId);

            std::shared_ptr<ComputeNode> retNode =
                currentGraph->GetNode(retNodeId);
            if (retNode) {
                retNode->containingFunc = callee;
                retNode->SetProperty("call_site_id", std::to_string(callNodeId));
                retNode->SetProperty("is_return_value", "true");
            }

            setLoopContext(retNodeId);

            ConnectNodes(retNodeId, callNodeId, ComputeEdgeKind::Return, "return");

            if (callNode) {
                callNode->SetProperty("return_node", std::to_string(retNodeId));
            }
        }
    }

    if (!hasExplicitReturn && !callee->getReturnType()->isVoidType()) {
        ComputeNode::NodeId lastExprNodeId =
            FindImplicitReturnValue(callee, callNodeId);

        if (lastExprNodeId != 0) {
            ConnectNodes(lastExprNodeId, callNodeId,
                        ComputeEdgeKind::Return, "implicit_return");
            returnNodeIds.push_back(lastExprNodeId);

            std::shared_ptr<ComputeNode> retNode =
                currentGraph->GetNode(lastExprNodeId);
            if (retNode) {
                retNode->SetProperty("is_return_value", "true");
            }

            if (callNode) {
                callNode->SetProperty("return_node", std::to_string(lastExprNodeId));
                callNode->SetProperty("implicit_return", "true");
            }
        }
    }

    for (ComputeNode::NodeId retNodeId : returnNodeIds) {
        std::shared_ptr<ComputeNode> retNode = currentGraph->GetNode(retNodeId);
        if (!retNode) {
            continue;
        }

        std::vector<std::shared_ptr<ComputeEdge>> incomingEdges =
            currentGraph->GetIncomingEdges(retNodeId);
        bool hasIncoming = !incomingEdges.empty();

        if (!hasIncoming) {
            for (const std::pair<const clang::Stmt*,
                                 ComputeNode::NodeId>& stmtPair : processedStmts) {
                if (stmtPair.second == retNodeId) {
                    TraceAllDefinitionsBackward(stmtPair.first, 1);
                    break;
                }
            }
        }
    }
}

ComputeNode::NodeId ComputeGraphBuilder::FindImplicitReturnValue(
    const clang::FunctionDecl* callee,
    ComputeNode::NodeId callNodeId)
{
    ComputeNode::NodeId lastExprNodeId = 0;

    const clang::CompoundStmt* compoundStmt =
        llvm::dyn_cast<clang::CompoundStmt>(callee->getBody());

    if (compoundStmt && !compoundStmt->body_empty()) {
        const clang::Stmt* lastStmt = compoundStmt->body_back();
        const clang::Expr* exprStmt = llvm::dyn_cast<clang::Expr>(lastStmt);

        if (exprStmt) {
            std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it =
                processedStmts.find(exprStmt);
            if (it != processedStmts.end()) {
                lastExprNodeId = it->second;
            }
        }
    }

    if (lastExprNodeId == 0) {
        const std::map<ComputeNode::NodeId,
                       std::shared_ptr<ComputeNode>>& nodes =
            currentGraph->GetNodes();

        for (const std::pair<const ComputeNode::NodeId,
                             std::shared_ptr<ComputeNode>>& nodePair : nodes) {
            ComputeNode::NodeId id = nodePair.first;
            const std::shared_ptr<ComputeNode>& node = nodePair.second;

            if (node->containingFunc != callee) {
                continue;
            }

            std::string nodeCallSiteId = node->GetProperty("call_site_id");
            if (nodeCallSiteId != std::to_string(callNodeId)) {
                continue;
            }

            if (node->kind == ComputeNodeKind::MemberAccess) {
                if (node->name.find(".f") != std::string::npos ||
                    node->GetProperty("is_union_member") == "true") {
                    lastExprNodeId = id;
                    break;
                }
            }
        }
    }

    return lastExprNodeId;
}

void ComputeGraphBuilder::PropagateContextToCalleeNodes(
    const clang::FunctionDecl* callee,
    ComputeNode::NodeId callNodeId,
    ComputeNode::NodeId inheritedLoopContextId,
    const std::string& inheritedLoopContextVar,
    int inheritedLoopContextLine)
{
    StmtCollector collector;
    collector.TraverseStmt(callee->getBody());

    for (const clang::Stmt* s : collector.stmts) {
        std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it =
            processedStmts.find(s);

        if (it != processedStmts.end()) {
            std::shared_ptr<ComputeNode> node = currentGraph->GetNode(it->second);

            if (node) {
                if (!node->containingFunc) {
                    node->containingFunc = callee;
                }

                if (!node->HasProperty("call_site_id") ||
                    node->GetProperty("call_site_id").empty()) {
                    node->SetProperty("call_site_id", std::to_string(callNodeId));
                }

                if (inheritedLoopContextId != 0 && node->loopContextId == 0) {
                    node->loopContextId = inheritedLoopContextId;
                    node->loopContextVar = inheritedLoopContextVar;
                    node->loopContextLine = inheritedLoopContextLine;
                    node->SetProperty("in_loop_context", "true");
                }
            }
        }
    }
}

    void ComputeGraphBuilder::TraceArgumentToDefinition(
    const clang::Expr* arg,
    ComputeNode::NodeId argNodeId,
    const clang::FunctionDecl* callerFunc)
{
    if (!arg || !callerFunc) {
        return;
    }


    VarRefExtractor extractor;
    extractor.TraverseStmt(const_cast<clang::Expr*>(arg));

    for (const clang::VarDecl* varDecl : extractor.varDecls) {

        DeclFinder declFinder(varDecl);
        declFinder.TraverseStmt(callerFunc->getBody());

        if (declFinder.foundDeclStmt) {
            ComputeNode::NodeId declNodeId =
                BuildExpressionTree(declFinder.foundDeclStmt, 0);

            if (declNodeId != 0) {
                std::shared_ptr<ComputeNode> declNode =
                    currentGraph->GetNode(declNodeId);

                if (declNode) {
                    declNode->containingFunc = callerFunc;
                }

                ConnectNodes(declNodeId, argNodeId, ComputeEdgeKind::DataFlow,
                           varDecl->getNameAsString());
            }
        }
    }
}

} // namespace compute_graph
