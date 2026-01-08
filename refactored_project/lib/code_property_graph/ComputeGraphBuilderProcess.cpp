/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphBuilderExpr.cpp - 表达式树构建（重构版）
 */

#include "ComputeGraph.h"
#include "code_property_graph/CPGAnnotation.h"
#include "code_property_graph/CPGBase.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"

namespace compute_graph {

// ============================================
// 辅助函数：类型判断
// ============================================

bool ComputeGraphBuilder::IsControlFlowStmt(const clang::Stmt* stmt)
{
    if (!stmt) return false;
    
    return llvm::isa<clang::IfStmt>(stmt) ||
           llvm::isa<clang::SwitchStmt>(stmt) ||
           llvm::isa<clang::ForStmt>(stmt) ||
           llvm::isa<clang::WhileStmt>(stmt) ||
           llvm::isa<clang::DoStmt>(stmt);
}

bool ComputeGraphBuilder::IsLoopStmt(const clang::Stmt* stmt)
{
    if (!stmt) return false;
    
    return llvm::isa<clang::ForStmt>(stmt) ||
           llvm::isa<clang::WhileStmt>(stmt) ||
           llvm::isa<clang::DoStmt>(stmt);
}

// ============================================
// 隐式转换处理
// ============================================

ComputeNode::NodeId ComputeGraphBuilder::HandleSimpleImplicitCast(
    const clang::ImplicitCastExpr* implCast, int depth)
{
    if (!implCast) return 0;
    
    clang::CastKind castKind = implCast->getCastKind();
    
    // 这些简单转换可以直接穿透
    if (castKind == clang::CK_LValueToRValue ||
        castKind == clang::CK_NoOp ||
        castKind == clang::CK_FunctionToPointerDecay ||
        castKind == clang::CK_ArrayToPointerDecay) {
        return BuildExpressionTree(implCast->getSubExpr(), depth);
    }
    
    return 0;  // 需要创建节点
}

// ============================================
// 控制流查找
// ============================================

const clang::Stmt* ComputeGraphBuilder::FindEnclosingControlFlow(
    const clang::Stmt* stmt)
{
    if (!stmt) return nullptr;
    
    const clang::Stmt* target = stmt;
    
    while (true) {
        clang::DynTypedNodeList parents = astContext.getParents(*target);
        if (parents.empty()) break;
        
        const clang::DynTypedNode& parentNode = parents[0];
        const clang::Stmt* parentStmt = parentNode.get<clang::Stmt>();
        const clang::Decl* parentDecl = parentNode.get<clang::Decl>();
        
        // 到达函数边界
        if (parentDecl && llvm::isa<clang::FunctionDecl>(parentDecl)) {
            break;
        }
        
        // 父节点已处理
        if (parentStmt && processedStmts.count(parentStmt)) {
            break;
        }
        
        // 找到控制流语句
        if (parentStmt && IsControlFlowStmt(parentStmt)) {
            return parentStmt;
        }
        
        if (parentStmt) {
            target = parentStmt;
        } else {
            break;
        }
    }
    
    return nullptr;
}

// ============================================
// 循环上下文应用
// ============================================

void ComputeGraphBuilder::ApplyLoopContext(
    std::shared_ptr<ComputeNode> node, const clang::Stmt* stmt)
{
    if (!node || !stmt) return;
    if (node->loopContextId != 0) return;  // 已设置
    
    const clang::Stmt* cursor = stmt;
    
    while (true) {
        clang::DynTypedNodeList parents = astContext.getParents(*cursor);
        if (parents.empty()) break;
        
        const clang::DynTypedNode& pNode = parents[0];
        
        // 到达函数边界
        if (const clang::Decl* pDecl = pNode.get<clang::Decl>()) {
            if (llvm::isa<clang::FunctionDecl>(pDecl)) break;
        }
        
        const clang::Stmt* pStmt = pNode.get<clang::Stmt>();
        if (!pStmt) break;
        
        // 检查是否是已处理的循环
        if (processedStmts.count(pStmt) && IsLoopStmt(pStmt)) {
            ComputeNode::NodeId loopId = processedStmts[pStmt];
            std::shared_ptr<ComputeNode> loopNode = currentGraph->GetNode(loopId);
            
            node->loopContextId = loopId;
            node->loopContextLine = loopNode ? loopNode->sourceLine : 0;
            
            std::string loopTag = "IN LOOP[" + std::to_string(loopId) + "]";
            node->SetProperty("loop_context", loopTag);
            
            break;
        }
        
        cursor = pStmt;
    }
}

// ============================================
// 二元运算符处理：复合赋值 (+=, -=等)
// ============================================

void ComputeGraphBuilder::HandleCompoundAssignment(
    const clang::BinaryOperator* binOp, ComputeNode::NodeId nodeId, int depth)
{
    if (!binOp) return;
    
    std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
    if (node) {
        node->SetProperty("is_compound_assign", "true");
    }
    
    // LHS: 先读后写
    if (const clang::Expr* lhs = binOp->getLHS()) {
        ComputeNode::NodeId lhsId = BuildExpressionTree(
            lhs->IgnoreParenImpCasts(), depth + 1);
        if (lhsId != 0) {
            // 读操作
            ConnectNodes(lhsId, nodeId, ComputeEdgeKind::DataFlow, "lhs_read");
            // 写操作
            ConnectNodes(nodeId, lhsId, ComputeEdgeKind::DataFlow, "assign_to");
            
            std::shared_ptr<ComputeNode> lhsNode = currentGraph->GetNode(lhsId);
            if (lhsNode) {
                lhsNode->SetProperty("is_assign_target", "true");
                lhsNode->SetProperty("is_read_write", "true");
            }
        }
    }
    
    // RHS: 只读
    if (const clang::Expr* rhs = binOp->getRHS()) {
        ComputeNode::NodeId rhsId = BuildExpressionTree(
            rhs->IgnoreParenImpCasts(), depth + 1);
        if (rhsId != 0) {
            ConnectNodes(rhsId, nodeId, ComputeEdgeKind::DataFlow, "rhs");
        }
    }
}

// ============================================
// 二元运算符处理：普通赋值 (=)
// ============================================

void ComputeGraphBuilder::HandleAssignment(
    const clang::BinaryOperator* binOp, ComputeNode::NodeId nodeId, int depth)
{
    if (!binOp) return;
    
    // RHS先处理
    if (const clang::Expr* rhs = binOp->getRHS()) {
        ComputeNode::NodeId rhsId = BuildExpressionTree(
            rhs->IgnoreParenImpCasts(), depth + 1);
        if (rhsId != 0) {
            ConnectNodes(rhsId, nodeId, ComputeEdgeKind::DataFlow, "rhs");
        }
    }
    
    // LHS: 只写
    if (const clang::Expr* lhs = binOp->getLHS()) {
        ComputeNode::NodeId lhsId = BuildExpressionTree(
            lhs->IgnoreParenImpCasts(), depth + 1);
        if (lhsId != 0) {
            ConnectNodes(nodeId, lhsId, ComputeEdgeKind::DataFlow, "assign_to");
            
            std::shared_ptr<ComputeNode> lhsNode = currentGraph->GetNode(lhsId);
            if (lhsNode) {
                lhsNode->SetProperty("is_assign_target", "true");
            }
        }
    }
}

// ============================================
// 二元运算符处理：普通运算 (+, -, *, /等)
// ============================================

void ComputeGraphBuilder::HandleNormalBinaryOp(
    const clang::BinaryOperator* binOp, ComputeNode::NodeId nodeId, int depth)
{
    if (!binOp) return;
    
    if (const clang::Expr* lhs = binOp->getLHS()) {
        ComputeNode::NodeId lhsId = BuildExpressionTree(
            lhs->IgnoreParenImpCasts(), depth + 1);
        if (lhsId != 0) {
            ConnectNodes(lhsId, nodeId, ComputeEdgeKind::DataFlow, "lhs");
        }
    }
    
    if (const clang::Expr* rhs = binOp->getRHS()) {
        ComputeNode::NodeId rhsId = BuildExpressionTree(
            rhs->IgnoreParenImpCasts(), depth + 1);
        if (rhsId != 0) {
            ConnectNodes(rhsId, nodeId, ComputeEdgeKind::DataFlow, "rhs");
        }
    }
}

// ============================================
// 二元运算符总调度
// ============================================

void ComputeGraphBuilder::ProcessBinaryOperator(
    const clang::BinaryOperator* binOp, ComputeNode::NodeId nodeId, int depth)
{
    if (!binOp) return;
    
    if (binOp->isCompoundAssignmentOp()) {
        HandleCompoundAssignment(binOp, nodeId, depth);
    } else if (binOp->isAssignmentOp()) {
        HandleAssignment(binOp, nodeId, depth);
    } else {
        HandleNormalBinaryOp(binOp, nodeId, depth);
    }
}

// ============================================
// 一元运算符处理
// ============================================

void ComputeGraphBuilder::ProcessUnaryOperator(
    const clang::UnaryOperator* unaryOp, ComputeNode::NodeId nodeId, int depth)
{
    if (!unaryOp) return;
    
    if (const clang::Expr* operand = unaryOp->getSubExpr()) {
        ComputeNode::NodeId operandId = BuildExpressionTree(
            operand->IgnoreParenImpCasts(), depth + 1);
        if (operandId != 0) {
            ConnectNodes(operandId, nodeId, ComputeEdgeKind::DataFlow, "operand");
        }
    }
}

// ============================================
// 数组下标处理
// ============================================

void ComputeGraphBuilder::ProcessArraySubscript(
    const clang::ArraySubscriptExpr* arrayExpr, 
    ComputeNode::NodeId nodeId, 
    int depth)
{
    if (!arrayExpr) return;
    
    if (const clang::Expr* base = arrayExpr->getBase()) {
        ComputeNode::NodeId baseId = BuildExpressionTree(
            base->IgnoreParenImpCasts(), depth + 1);
        if (baseId != 0) {
            ConnectNodes(baseId, nodeId, ComputeEdgeKind::DataFlow, "base");
        }
    }
    
    if (const clang::Expr* idx = arrayExpr->getIdx()) {
        ComputeNode::NodeId idxId = BuildExpressionTree(
            idx->IgnoreParenImpCasts(), depth + 1);
        if (idxId != 0) {
            ConnectNodes(idxId, nodeId, ComputeEdgeKind::DataFlow, "index");
        }
    }
}

// ============================================
// 构造函数调用处理
// ============================================

void ComputeGraphBuilder::ProcessConstructorExpr(
    const clang::CXXConstructExpr* ctorExpr, 
    ComputeNode::NodeId nodeId, 
    int depth)
{
    if (!ctorExpr) return;
    
    if (ctorExpr->getNumArgs() > 0) {
        const clang::Expr* arg = ctorExpr->getArg(0);
        ComputeNode::NodeId argId = BuildExpressionTree(
            arg->IgnoreParenImpCasts(), depth + 1);
        if (argId != 0) {
            ConnectNodes(argId, nodeId, ComputeEdgeKind::DataFlow, "ctor_arg");
        }
    }
}

// ============================================
// 函数调用：处理参数
// ============================================

void ComputeGraphBuilder::ProcessCallArguments(
    const clang::CallExpr* callExpr, ComputeNode::NodeId nodeId, int depth)
{
    if (!callExpr) return;
    
    int argIdx = 0;
    for (const clang::Expr* arg : callExpr->arguments()) {
        ComputeNode::NodeId argId = BuildExpressionTree(
            arg->IgnoreParenImpCasts(), depth + 1);
        if (argId != 0) {
            std::string label = "arg" + std::to_string(argIdx);
            ConnectNodes(argId, nodeId, ComputeEdgeKind::DataFlow, label);
        }
        argIdx++;
    }
}

// ============================================
// 函数调用：跨函数分析
// ============================================

void ComputeGraphBuilder::ProcessCalleeAnalysis(
    const clang::CallExpr* callExpr, ComputeNode::NodeId nodeId)
{
    if (!callExpr) return;
    if (!enableInterprocedural || currentCallDepth >= maxCallDepth) {
        return;
    }
    
    const clang::FunctionDecl* callee = callExpr->getDirectCallee();
    if (!callee || !callee->hasBody()) {
        return;
    }
    
    // 检查是否是向量化内置函数
    const clang::SourceManager& sm = astContext.getSourceManager();
    if (IsVectorIntrinsicFunction(callee, sm)) {
        std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
        if (node) {
            node->SetProperty("is_intrinsic", "true");
        }
        return;
    }
    
    // 检查递归
    bool isRecursive = (currentCallStack.find(callee->getCanonicalDecl()) != 
                       currentCallStack.end());
    if (isRecursive) {
        return;
    }
    
    // 进入被调用函数分析
    currentCallStack.insert(callee->getCanonicalDecl());
    currentCallDepth++;
    AnalyzeCalleeBody(callee, nodeId, callExpr);
    currentCallDepth--;
    currentCallStack.erase(callee->getCanonicalDecl());
}

// ============================================
// 函数调用总处理
// ============================================

void ComputeGraphBuilder::ProcessCallExpr(
    const clang::CallExpr* callExpr, ComputeNode::NodeId nodeId, int depth)
{
    if (!callExpr) return;
    
    ProcessCallArguments(callExpr, nodeId, depth);
    ProcessCalleeAnalysis(callExpr, nodeId);
}

// ============================================
// 类型转换处理
// ============================================

void ComputeGraphBuilder::ProcessCastExpr(
    const clang::CastExpr* castExpr, ComputeNode::NodeId nodeId, int depth)
{
    if (!castExpr) return;
    
    if (const clang::Expr* subExpr = castExpr->getSubExpr()) {
        ComputeNode::NodeId subId = BuildExpressionTree(
            subExpr->IgnoreParenImpCasts(), depth + 1);
        if (subId != 0) {
            ConnectNodes(subId, nodeId, ComputeEdgeKind::DataFlow, "cast");
        }
    }
}

// ============================================
// 临时对象处理
// ============================================

void ComputeGraphBuilder::ProcessMaterializeTemporaryExpr(
    const clang::MaterializeTemporaryExpr* matTemp, 
    ComputeNode::NodeId nodeId, 
    int depth)
{
    if (!matTemp) return;
    
    if (const clang::Expr* subExpr = matTemp->getSubExpr()) {
        ComputeNode::NodeId subId = BuildExpressionTree(
            subExpr->IgnoreParenImpCasts(), depth + 1);
        if (subId != 0) {
            ConnectNodes(subId, nodeId, ComputeEdgeKind::DataFlow, "temp");
        }
    }
}

// ============================================
// 成员访问：Union成员特殊处理
// ============================================

void ComputeGraphBuilder::HandleUnionMemberAccess(
    ComputeNode::NodeId baseId,
    ComputeNode::NodeId nodeId,
    const clang::FieldDecl* fieldDecl,
    const clang::RecordDecl* recordDecl,
    const clang::Expr* baseExpr)
{
    std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
    if (!node) return;
    
    node->SetProperty("is_union_member", "true");
    node->SetProperty("union_base_id", std::to_string(baseId));
    
    // 获取union变量名
    std::string unionVarName;
    std::shared_ptr<ComputeNode> baseNode = currentGraph->GetNode(baseId);
    if (baseNode && !baseNode->name.empty()) {
        unionVarName = baseNode->name;
    } else {
        const clang::Expr* base = baseExpr->IgnoreParenImpCasts();
        if (const clang::DeclRefExpr* declRef = 
            llvm::dyn_cast<clang::DeclRefExpr>(base)) {
            unionVarName = declRef->getDecl()->getNameAsString();
        }
    }
    
    if (!unionVarName.empty()) {
        node->SetProperty("union_var", unionVarName);
        node->name = unionVarName + "." + fieldDecl->getNameAsString();
    }
    
    // 传递call_site_id属性
    if (baseNode && baseNode->HasProperty("call_site_id")) {
        node->SetProperty("call_site_id", 
            baseNode->GetProperty("call_site_id"));
    }
    
    ConnectUnionAliases(baseId, nodeId, recordDecl, fieldDecl);
    ConnectNodes(baseId, nodeId, ComputeEdgeKind::DataFlow, "union_member");
}

// ============================================
// 成员访问处理
// ============================================

void ComputeGraphBuilder::ProcessMemberExpr(
    const clang::MemberExpr* memberExpr, ComputeNode::NodeId nodeId, int depth)
{
    if (!memberExpr) return;
    
    const clang::Expr* base = memberExpr->getBase();
    if (!base) return;
    
    ComputeNode::NodeId baseId = BuildExpressionTree(
        base->IgnoreParenImpCasts(), depth + 1);
    if (baseId == 0) return;
    
    const clang::ValueDecl* memberDecl = memberExpr->getMemberDecl();
    const clang::FieldDecl* fieldDecl = 
        llvm::dyn_cast<clang::FieldDecl>(memberDecl);
    
    if (!fieldDecl) {
        ConnectNodes(baseId, nodeId, ComputeEdgeKind::DataFlow, "base");
        return;
    }
    
    const clang::RecordDecl* recordDecl = fieldDecl->getParent();
    if (recordDecl && recordDecl->isUnion()) {
        // Union成员需要特殊处理
        HandleUnionMemberAccess(baseId, nodeId, fieldDecl, recordDecl, base);
    } else {
        // 普通成员访问
        ConnectNodes(baseId, nodeId, ComputeEdgeKind::DataFlow, "base");
    }
}

// ============================================
// For循环处理
// ============================================

void ComputeGraphBuilder::ProcessForStmt(
    const clang::ForStmt* forStmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!forStmt) return;
    
    if (const clang::Stmt* init = forStmt->getInit()) {
        ComputeNode::NodeId initId = BuildExpressionTree(init, depth + 1);
        if (initId != 0) {
            ConnectNodes(initId, nodeId, ComputeEdgeKind::Control, "init");
        }
    }
    
    if (const clang::Expr* cond = forStmt->getCond()) {
        ComputeNode::NodeId condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, nodeId, ComputeEdgeKind::Control, "condition");
        }
    }
    
    if (const clang::Expr* inc = forStmt->getInc()) {
        ComputeNode::NodeId incId = BuildExpressionTree(inc, depth + 1);
        if (incId != 0) {
            ConnectNodes(incId, nodeId, ComputeEdgeKind::Control, "increment");
        }
    }
}

// ============================================
// While循环处理
// ============================================

void ComputeGraphBuilder::ProcessWhileStmt(
    const clang::WhileStmt* whileStmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!whileStmt) return;
    
    if (const clang::Expr* cond = whileStmt->getCond()) {
        ComputeNode::NodeId condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, nodeId, ComputeEdgeKind::Control, "condition");
        }
    }
}

// ============================================
// Do-While循环处理
// ============================================

void ComputeGraphBuilder::ProcessDoStmt(
    const clang::DoStmt* doStmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!doStmt) return;
    
    if (const clang::Expr* cond = doStmt->getCond()) {
        ComputeNode::NodeId condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, nodeId, ComputeEdgeKind::Control, "condition");
        }
    }
}

// ============================================
// 三元运算符处理
// ============================================

void ComputeGraphBuilder::ProcessConditionalOperator(
    const clang::ConditionalOperator* condOp, 
    ComputeNode::NodeId nodeId, 
    int depth)
{
    if (!condOp) return;
    
    if (const clang::Expr* cond = condOp->getCond()) {
        ComputeNode::NodeId condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, nodeId, ComputeEdgeKind::Control, "condition");
        }
    }
    
    if (const clang::Expr* trueExpr = condOp->getTrueExpr()) {
        ComputeNode::NodeId trueId = BuildExpressionTree(trueExpr, depth + 1);
        if (trueId != 0) {
            ConnectNodes(trueId, nodeId, ComputeEdgeKind::DataFlow, "true_val");
        }
    }
    
    if (const clang::Expr* falseExpr = condOp->getFalseExpr()) {
        ComputeNode::NodeId falseId = BuildExpressionTree(falseExpr, depth + 1);
        if (falseId != 0) {
            ConnectNodes(falseId, nodeId, ComputeEdgeKind::DataFlow, "false_val");
        }
    }
}

// ============================================
// Return语句处理
// ============================================

void ComputeGraphBuilder::ProcessReturnStmt(
    const clang::ReturnStmt* retStmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!retStmt) return;
    
    if (const clang::Expr* retValue = retStmt->getRetValue()) {
        ComputeNode::NodeId childId = BuildExpressionTree(
            retValue->IgnoreParenImpCasts(), depth + 1);
        if (childId != 0) {
            ConnectNodes(childId, nodeId, ComputeEdgeKind::DataFlow, "child");
        }
    }
}

// ============================================
// 声明语句处理
// ============================================

void ComputeGraphBuilder::ProcessDeclStmt(
    const clang::DeclStmt* declStmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!declStmt) return;
    
    for (const clang::Decl* decl : declStmt->decls()) {
        const clang::VarDecl* varDecl = llvm::dyn_cast<clang::VarDecl>(decl);
        if (!varDecl || !varDecl->hasInit()) {
            continue;
        }
        
        const clang::Expr* initExpr = varDecl->getInit();
        if (!initExpr) {
            continue;
        }
        
        ComputeNode::NodeId initId = BuildExpressionTree(
            initExpr->IgnoreParenImpCasts(), depth + 1);
        if (initId != 0) {
            ConnectNodes(initId, nodeId, ComputeEdgeKind::DataFlow, "init");
        }
    }
}

// ============================================
// 通用子节点处理
// ============================================

void ComputeGraphBuilder::ProcessGenericChildren(
    const clang::Stmt* stmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!stmt) return;
    
    for (const clang::Stmt* child : stmt->children()) {
        if (!child) continue;
        
        ComputeNode::NodeId childId = BuildExpressionTree(child, depth + 1);
        if (childId != 0) {
            ConnectNodes(childId, nodeId, ComputeEdgeKind::DataFlow, "child");
        }
    }
}

// ============================================
// 子节点处理总调度
// ============================================

void ComputeGraphBuilder::ProcessStatementChildren(
    const clang::Stmt* stmt, ComputeNode::NodeId nodeId, int depth)
{
    if (!stmt) return;
    
    // 使用dynamic_cast分发到具体处理函数
    if (const clang::BinaryOperator* binOp = 
        llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
        ProcessBinaryOperator(binOp, nodeId, depth);
    }
    else if (const clang::UnaryOperator* unaryOp = 
             llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
        ProcessUnaryOperator(unaryOp, nodeId, depth);
    }
    else if (const clang::ArraySubscriptExpr* arrayExpr = 
             llvm::dyn_cast<clang::ArraySubscriptExpr>(stmt)) {
        ProcessArraySubscript(arrayExpr, nodeId, depth);
    }
    else if (const clang::CXXConstructExpr* ctorExpr = 
             llvm::dyn_cast<clang::CXXConstructExpr>(stmt)) {
        ProcessConstructorExpr(ctorExpr, nodeId, depth);
    }
    else if (const clang::CallExpr* callExpr = 
             llvm::dyn_cast<clang::CallExpr>(stmt)) {
        ProcessCallExpr(callExpr, nodeId, depth);
    }
    else if (const clang::CastExpr* castExpr = 
             llvm::dyn_cast<clang::CastExpr>(stmt)) {
        ProcessCastExpr(castExpr, nodeId, depth);
    }
    else if (const clang::MaterializeTemporaryExpr* matTemp = 
             llvm::dyn_cast<clang::MaterializeTemporaryExpr>(stmt)) {
        ProcessMaterializeTemporaryExpr(matTemp, nodeId, depth);
    }
    else if (const clang::ImplicitCastExpr* implCast = 
             llvm::dyn_cast<clang::ImplicitCastExpr>(stmt)) {
        // 隐式转换在这里处理子表达式
        if (const clang::Expr* subExpr = implCast->getSubExpr()) {
            ComputeNode::NodeId subId = BuildExpressionTree(
                subExpr->IgnoreParenImpCasts(), depth + 1);
            if (subId != 0) {
                ConnectNodes(subId, nodeId, ComputeEdgeKind::DataFlow, "implicit");
            }
        }
    }
    else if (const clang::MemberExpr* memberExpr = 
             llvm::dyn_cast<clang::MemberExpr>(stmt)) {
        ProcessMemberExpr(memberExpr, nodeId, depth);
    }
    else if (llvm::isa<clang::DeclRefExpr>(stmt)) {
        // 叶子节点，无需处理子节点
    }
    else if (const clang::ForStmt* forStmt = 
             llvm::dyn_cast<clang::ForStmt>(stmt)) {
        ProcessForStmt(forStmt, nodeId, depth);
    }
    else if (const clang::WhileStmt* whileStmt = 
             llvm::dyn_cast<clang::WhileStmt>(stmt)) {
        ProcessWhileStmt(whileStmt, nodeId, depth);
    }
    else if (const clang::DoStmt* doStmt = 
             llvm::dyn_cast<clang::DoStmt>(stmt)) {
        ProcessDoStmt(doStmt, nodeId, depth);
    }
    else if (const clang::ConditionalOperator* condOp = 
             llvm::dyn_cast<clang::ConditionalOperator>(stmt)) {
        ProcessConditionalOperator(condOp, nodeId, depth);
    }
    else if (const clang::ReturnStmt* retStmt = 
             llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        ProcessReturnStmt(retStmt, nodeId, depth);
    }
    else if (const clang::DeclStmt* declStmt = 
             llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        ProcessDeclStmt(declStmt, nodeId, depth);
    }
    else {
        // 其他类型使用通用处理
        ProcessGenericChildren(stmt, nodeId, depth);
    }
}

    // 获取语句所在的函数
    const clang::FunctionDecl* ComputeGraphBuilder::GetContainingFunction(
        const clang::Stmt* stmt) const
{
    if (!stmt) return nullptr;

    auto parents = astContext.getParents(*stmt);
    while (!parents.empty()) {
        const auto& parent = parents[0];

        if (auto* funcDecl = parent.get<clang::FunctionDecl>()) {
            return funcDecl;
        }

        if (auto* pStmt = parent.get<clang::Stmt>()) {
            parents = astContext.getParents(*pStmt);
        } else if (auto* pDecl = parent.get<clang::Decl>()) {
            parents = astContext.getParents(*pDecl);
        } else {
            break;
        }
    }

    return nullptr;
}

// ============================================
// 主函数：BuildExpressionTree
// ============================================

ComputeNode::NodeId ComputeGraphBuilder::BuildExpressionTree(
    const clang::Stmt* stmt, int depth)
{
    if (!stmt) return 0;
    if (depth > maxExprDepth) return 0;
    
    // 1. 处理简单隐式转换
    if (const clang::ImplicitCastExpr* implCast = 
        llvm::dyn_cast<clang::ImplicitCastExpr>(stmt)) {
        ComputeNode::NodeId result = HandleSimpleImplicitCast(implCast, depth);
        if (result != 0) return result;
    }
    
    // 2. 缓存检查
    std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it = 
        processedStmts.find(stmt);
    if (it != processedStmts.end()) {
        return it->second;
    }
    
    // 3. 控制流提升
    const clang::Stmt* enclosingControl = FindEnclosingControlFlow(stmt);
    if (enclosingControl) {
        BuildExpressionTree(enclosingControl, depth);
        if (processedStmts.count(stmt)) {
            return processedStmts[stmt];
        }
    }
    
    // 4. 特殊分支结构优先处理
    if (const clang::IfStmt* ifStmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
        return BuildIfBranch(ifStmt, depth);
    }
    if (const clang::SwitchStmt* switchStmt = 
        llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
        return BuildSwitchBranch(switchStmt, depth);
    }
    
    // 5. 创建节点
    ComputeNode::NodeId nodeId = CreateNodeFromStmt(stmt);
    std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
    if (!node) return 0;
    
    node->sourceText = GetSourceText(stmt, astContext);
    node->sourceLine = GetSourceLine(stmt, astContext);
    
    // 6. 立即注册到缓存
    processedStmts[stmt] = nodeId;
    
    // 7. 应用循环上下文
    ApplyLoopContext(node, stmt);
    
    // 8. 处理子节点
    ProcessStatementChildren(stmt, nodeId, depth);
    
    return nodeId;
}

    void ComputeGraphBuilder::ConnectUnionAliases(
    ComputeNode::NodeId baseId,
    ComputeNode::NodeId currentMemberId,
    const clang::RecordDecl* unionDecl,
    const clang::FieldDecl* currentField)
{
    if (!unionDecl || !currentField) {
        return;
    }

    std::string currentFieldName = currentField->getNameAsString();
    std::shared_ptr<ComputeNode> currentNode =
        currentGraph->GetNode(currentMemberId);

    if (!currentNode) {
        return;
    }

    std::string currentUnionVar = currentNode->GetProperty("union_var");
    std::string currentCallSiteId = currentNode->GetProperty("call_site_id");
    bool currentIsWriteTarget =
        (currentNode->GetProperty("is_assign_target") == "true");

    const std::map<ComputeNode::NodeId,
                   std::shared_ptr<ComputeNode>>& nodes =
        currentGraph->GetNodes();

    for (const std::pair<const ComputeNode::NodeId,
                         std::shared_ptr<ComputeNode>>& nodePair : nodes) {
        ComputeNode::NodeId id = nodePair.first;
        const std::shared_ptr<ComputeNode>& node = nodePair.second;

        if (id == currentMemberId) {
            continue;
        }

        if (node->GetProperty("is_union_member") != "true") {
            continue;
        }

        std::string otherUnionVar = node->GetProperty("union_var");
        if (otherUnionVar.empty() || otherUnionVar != currentUnionVar) {
            continue;
        }

        std::string otherCallSiteId = node->GetProperty("call_site_id");

        if (!currentCallSiteId.empty() && !otherCallSiteId.empty()) {
            if (currentCallSiteId != otherCallSiteId) {
                continue;
            }
        } else if (!currentCallSiteId.empty() || !otherCallSiteId.empty()) {
            continue;
        } else {
            if (currentNode->containingFunc != node->containingFunc) {
                continue;
            }
        }

        std::string otherFieldName = node->name;
        size_t dotPos = otherFieldName.find_last_of('.');
        if (dotPos != std::string::npos) {
            otherFieldName = otherFieldName.substr(dotPos + 1);
        }

        if (otherFieldName == currentFieldName) {
            continue;
        }

        bool otherIsWriteTarget =
            (node->GetProperty("is_assign_target") == "true");

        if (currentIsWriteTarget && !otherIsWriteTarget) {
            std::string label = "union(" + currentFieldName + "->" +
                              otherFieldName + ")";
            ConnectNodes(currentMemberId, id, ComputeEdgeKind::Memory, label);
        } else if (!currentIsWriteTarget && otherIsWriteTarget) {
            std::string label = "union(" + otherFieldName + "->" +
                              currentFieldName + ")";
            ConnectNodes(id, currentMemberId, ComputeEdgeKind::Memory, label);
        } else {
            if (id < currentMemberId) {
                std::string label = "union(" + otherFieldName + "<->" +
                                  currentFieldName + ")";
                ConnectNodes(id, currentMemberId, ComputeEdgeKind::Memory, label);
            }
        }
    }
}

bool ComputeGraphBuilder::CheckIntermediateDefinitions(
    const clang::Stmt* defStmt,
    const clang::Stmt* useStmt,
    const std::string& varName) const
{
    int defLine = GetSourceLine(defStmt, astContext);
    int useLine = GetSourceLine(useStmt, astContext);

    const clang::FunctionDecl* func = GetContainingFunction(defStmt);
    if (!func || !func->hasBody()) {
        return false;
    }

    IntermediateDefFinder finder(varName, defLine, useLine, astContext);
    finder.TraverseStmt(func->getBody());

    return finder.foundIntermediate;
}

    ComputeNode::NodeId ComputeGraphBuilder::BuildSwitchBranch(
    const clang::SwitchStmt* switchStmt, int depth)
{
    if (!switchStmt || depth >= maxBackwardDepth) {
        return 0;
    }

    std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it =
        processedStmts.find(switchStmt);
    if (it != processedStmts.end()) {
        return it->second;
    }

    std::shared_ptr<ComputeNode> switchNode =
        currentGraph->CreateNode(ComputeNodeKind::Branch);
    switchNode->name = "switch";
    switchNode->sourceText = "switch (" +
        GetSourceText(switchStmt->getCond(), astContext) + ")";
    switchNode->SetProperty("branch_type", "switch");

    ComputeNode::NodeId switchId = switchNode->id;
    processedStmts[switchStmt] = switchId;

    const clang::Expr* cond = switchStmt->getCond();
    if (cond) {
        ComputeNode::NodeId condId = BuildExpressionTree(cond, depth + 1);
        if (condId != 0) {
            ConnectNodes(condId, switchId, ComputeEdgeKind::Control, "condition");
        }
    }

    const clang::CompoundStmt* body =
        llvm::dyn_cast<clang::CompoundStmt>(switchStmt->getBody());

    if (body) {
        ProcessSwitchBody(body, switchId, depth);
    } else {
        ProcessSwitchCasesSimple(switchStmt->getBody(), switchId, depth);
    }

    return switchId;
}

void ComputeGraphBuilder::ProcessSwitchBody(
    const clang::CompoundStmt* body,
    ComputeNode::NodeId switchId,
    int depth)
{
    BranchInfo switchInfo;
    switchInfo.branchNodeId = switchId;
    switchInfo.branchLine = GetSourceLine(body, astContext);

    std::string currentLabel = "";

    for (const clang::Stmt* s : body->body()) {
        if (processedStmts.count(s)) {
            continue;
        }

        const clang::CaseStmt* caseStmt = llvm::dyn_cast<clang::CaseStmt>(s);
        if (caseStmt) {
            currentLabel = "CASE " +
                GetSourceText(caseStmt->getLHS(), astContext);
        } else if (llvm::isa<clang::DefaultStmt>(s)) {
            currentLabel = "DEFAULT";
        }

        BuildExpressionTree(s, depth + 1);

        if (!currentLabel.empty()) {
            switchInfo.branchType = currentLabel;
            switchInfo.bodyStartLine = GetSourceLine(s, astContext);
            switchInfo.bodyEndLine = switchInfo.bodyStartLine;
            MarkNodesInBranch(switchInfo);
        }
    }
}

void ComputeGraphBuilder::ProcessSwitchCasesSimple(
    const clang::Stmt* body,
    ComputeNode::NodeId switchId,
    int depth)
{
    BuildExpressionTree(body, depth + 1);
}

} // namespace compute_graph
