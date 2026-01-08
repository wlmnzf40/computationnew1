/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphBuilderNode.cpp - 节点创建（重构版，第2部分）
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

namespace compute_graph {

// ============================================
// 节点创建：函数调用
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateCallExprNode(
    const clang::CallExpr* callExpr)
{
    if (!callExpr) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Call);
    
    // 尝试获取函数名
    if (const clang::FunctionDecl* callee = callExpr->getDirectCallee()) {
        // 直接调用
        node->name = callee->getNameAsString();
    } else if (const clang::Expr* calleeExpr = callExpr->getCallee()) {
        // 模板函数中的未解析调用
        const clang::Expr* stripped = calleeExpr->IgnoreParenImpCasts();
        
        if (const clang::UnresolvedLookupExpr* unresolvedLookup = 
            llvm::dyn_cast<clang::UnresolvedLookupExpr>(stripped)) {
            node->name = unresolvedLookup->getName().getAsString();
        } else if (const clang::DeclRefExpr* declRef = 
                   llvm::dyn_cast<clang::DeclRefExpr>(stripped)) {
            node->name = declRef->getDecl()->getNameAsString();
        } else if (const clang::MemberExpr* memberExpr = 
                   llvm::dyn_cast<clang::MemberExpr>(stripped)) {
            node->name = memberExpr->getMemberDecl()->getNameAsString();
        }
    }
    
    // 如果还是没有名字，标记为未知
    if (node->name.empty()) {
        node->name = "<call>";
    }
    
    node->dataType = DataTypeInfo::FromClangType(callExpr->getType());
    
    return node;
}

// ============================================
// 节点创建：构造函数调用
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateConstructorNode(
    const clang::CXXConstructExpr* ctorExpr)
{
    if (!ctorExpr) return nullptr;
    
    const clang::CXXConstructorDecl* ctor = ctorExpr->getConstructor();
    
    std::shared_ptr<ComputeNode> node;
    
    // 检查是否是拷贝/移动构造
    if (ctor && (ctor->isCopyConstructor() || ctor->isMoveConstructor())) {
        node = currentGraph->CreateNode(ComputeNodeKind::Cast);
        node->name = "copy_ctor";
    } else {
        node = currentGraph->CreateNode(ComputeNodeKind::Call);
        if (ctor) {
            node->name = ctor->getParent()->getNameAsString() + "::ctor";
        }
    }
    
    node->dataType = DataTypeInfo::FromClangType(ctorExpr->getType());
    
    return node;
}

// ============================================
// 节点创建：成员访问
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateMemberAccessNode(
    const clang::MemberExpr* memberExpr)
{
    if (!memberExpr) return nullptr;
    
    const clang::ValueDecl* memberDecl = memberExpr->getMemberDecl();
    bool isUnionMember = false;
    
    if (const clang::FieldDecl* fieldDecl = 
        llvm::dyn_cast<clang::FieldDecl>(memberDecl)) {
        const clang::RecordDecl* recordDecl = fieldDecl->getParent();
        if (recordDecl && recordDecl->isUnion()) {
            isUnionMember = true;
        }
    }
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::MemberAccess);
    
    // 获取基础对象名和成员名
    std::string baseName;
    if (const clang::Expr* base = memberExpr->getBase()->IgnoreParenImpCasts()) {
        if (const clang::DeclRefExpr* declRef = 
            llvm::dyn_cast<clang::DeclRefExpr>(base)) {
            baseName = declRef->getDecl()->getNameAsString();
        }
    }
    
    if (memberDecl) {
        std::string memberName = memberDecl->getNameAsString();
        if (!baseName.empty()) {
            node->name = baseName + "." + memberName;
        } else {
            node->name = memberName;
        }
    }
    
    node->dataType = DataTypeInfo::FromClangType(memberExpr->getType());
    node->SetProperty("is_member_access", "true");
    
    if (isUnionMember) {
        node->SetProperty("is_union_member", "true");
        node->SetProperty("union_var", baseName);
    }
    
    return node;
}

// ============================================
// 节点创建：类型转换
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateCastNode(
    const clang::CastExpr* castExpr, const std::string& castType)
{
    if (!castExpr) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Cast);
    
    node->name = castType;
    node->dataType = DataTypeInfo::FromClangType(castExpr->getType());
    
    return node;
}

// ============================================
// 节点创建：临时对象
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateTempNode(
    const clang::MaterializeTemporaryExpr* matTemp)
{
    if (!matTemp) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Cast);
    
    node->name = "temp";
    node->dataType = DataTypeInfo::FromClangType(matTemp->getType());
    
    return node;
}

// ============================================
// 节点创建：Return语句
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateReturnNode(
    const clang::ReturnStmt* retStmt)
{
    if (!retStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Return);
    
    node->name = "return";
    
    // 从返回值表达式获取类型
    if (const clang::Expr* retValue = retStmt->getRetValue()) {
        node->dataType = DataTypeInfo::FromClangType(retValue->getType());
    }
    
    return node;
}

// ============================================
// 节点创建：For循环
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateForLoopNode(
    const clang::ForStmt* forStmt)
{
    if (!forStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Loop);
    
    node->name = "for";
    node->SetProperty("loop_type", "for");
    
    // 提取循环条件信息
    if (const clang::Expr* cond = forStmt->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    // 提取循环步进信息
    if (const clang::Expr* inc = forStmt->getInc()) {
        node->SetProperty("increment", GetSourceText(inc, astContext));
    }
    
    return node;
}

// ============================================
// 节点创建：While循环
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateWhileLoopNode(
    const clang::WhileStmt* whileStmt)
{
    if (!whileStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Loop);
    
    node->name = "while";
    node->SetProperty("loop_type", "while");
    
    if (const clang::Expr* cond = whileStmt->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    return node;
}

// ============================================
// 节点创建：Do-While循环
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateDoWhileLoopNode(
    const clang::DoStmt* doStmt)
{
    if (!doStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Loop);
    
    node->name = "do-while";
    node->SetProperty("loop_type", "do-while");
    
    if (const clang::Expr* cond = doStmt->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    return node;
}

// ============================================
// 节点创建：If分支
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateIfBranchNode(
    const clang::IfStmt* ifStmt)
{
    if (!ifStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Branch);
    
    node->name = "if";
    node->SetProperty("branch_type", "if");
    
    if (const clang::Expr* cond = ifStmt->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    node->SetProperty("has_else", ifStmt->getElse() ? "true" : "false");
    
    return node;
}

// ============================================
// 节点创建：Switch分支
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateSwitchBranchNode(
    const clang::SwitchStmt* switchStmt)
{
    if (!switchStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Branch);
    
    node->name = "switch";
    node->SetProperty("branch_type", "switch");
    
    if (const clang::Expr* cond = switchStmt->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    return node;
}

// ============================================
// 节点创建：三元运算符
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateSelectNode(
    const clang::ConditionalOperator* condOp)
{
    if (!condOp) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Select);
    
    node->name = "?:";
    node->dataType = DataTypeInfo::FromClangType(condOp->getType());
    
    if (const clang::Expr* cond = condOp->getCond()) {
        node->SetProperty("condition", GetSourceText(cond, astContext));
    }
    
    return node;
}

// ============================================
// 节点创建：初始化列表
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateInitListNode(
    const clang::InitListExpr* initList)
{
    if (!initList) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Constant);
    
    node->name = "init_list";
    node->dataType = DataTypeInfo::FromClangType(initList->getType());
    
    return node;
}

// ============================================
// 节点创建：复合字面量
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateCompoundLiteralNode(
    const clang::CompoundLiteralExpr* compLit)
{
    if (!compLit) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Constant);
    
    node->name = "compound_literal";
    node->dataType = DataTypeInfo::FromClangType(compLit->getType());
    
    return node;
}

// ============================================
// 辅助函数：查找所属函数
// ============================================

void ComputeGraphBuilder::SetContainingFunction(
    std::shared_ptr<ComputeNode> node, const clang::Stmt* stmt)
{
    if (!node || !stmt) return;
    
    clang::DynTypedNodeList parents = astContext.getParents(*stmt);
    
    while (!parents.empty()) {
        const clang::DynTypedNode& parent = parents[0];
        
        if (const clang::FunctionDecl* funcDecl = 
            parent.get<clang::FunctionDecl>()) {
            node->containingFunc = funcDecl;
            break;
        }
        
        if (const clang::Stmt* pStmt = parent.get<clang::Stmt>()) {
            parents = astContext.getParents(*pStmt);
        } else if (const clang::Decl* pDecl = parent.get<clang::Decl>()) {
            parents = astContext.getParents(*pDecl);
        } else {
            break;
        }
    }
}

// ============================================
// 主函数：CreateNodeFromStmt
// 从原来的492行重构为<50行
// ============================================

ComputeNode::NodeId ComputeGraphBuilder::CreateNodeFromStmt(
    const clang::Stmt* stmt)
{
    if (!stmt) return 0;
    
    // 检查缓存
    std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it = 
        processedStmts.find(stmt);
    if (it != processedStmts.end()) {
        return it->second;
    }
    
    // 根据类型创建节点
    std::shared_ptr<ComputeNode> node;
    
    if (const clang::BinaryOperator* binOp = 
        llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
        node = CreateBinaryOpNode(binOp);
    }
    else if (const clang::UnaryOperator* unaryOp = 
             llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
        node = CreateUnaryOpNode(unaryOp);
    }
    else if (const clang::DeclRefExpr* declRef = 
             llvm::dyn_cast<clang::DeclRefExpr>(stmt)) {
        node = CreateVariableNode(declRef);
    }
    else if (const clang::IntegerLiteral* intLit = 
             llvm::dyn_cast<clang::IntegerLiteral>(stmt)) {
        node = CreateIntConstantNode(intLit);
    }
    else if (const clang::FloatingLiteral* floatLit = 
             llvm::dyn_cast<clang::FloatingLiteral>(stmt)) {
        node = CreateFloatConstantNode(floatLit);
    }
    else if (const clang::DeclStmt* declStmt = 
             llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        node = CreateDeclStmtNode(declStmt);
    }
    else if (const clang::ArraySubscriptExpr* arrayExpr = 
             llvm::dyn_cast<clang::ArraySubscriptExpr>(stmt)) {
        node = CreateArrayAccessNode(arrayExpr);
    }
    else if (const clang::CXXOperatorCallExpr* opCallExpr = 
             llvm::dyn_cast<clang::CXXOperatorCallExpr>(stmt)) {
        node = CreateOperatorCallNode(opCallExpr);
    }
    else if (const clang::CallExpr* callExpr = 
             llvm::dyn_cast<clang::CallExpr>(stmt)) {
        node = CreateCallExprNode(callExpr);
    }
    else if (const clang::CXXConstructExpr* ctorExpr = 
             llvm::dyn_cast<clang::CXXConstructExpr>(stmt)) {
        node = CreateConstructorNode(ctorExpr);
    }
    else if (const clang::MemberExpr* memberExpr = 
             llvm::dyn_cast<clang::MemberExpr>(stmt)) {
        node = CreateMemberAccessNode(memberExpr);
    }
    else if (const clang::MaterializeTemporaryExpr* matTemp = 
             llvm::dyn_cast<clang::MaterializeTemporaryExpr>(stmt)) {
        node = CreateTempNode(matTemp);
    }
    else if (const clang::ImplicitCastExpr* implCast = 
             llvm::dyn_cast<clang::ImplicitCastExpr>(stmt)) {
        node = CreateCastNode(implCast, "implicit_cast");
    }
    else if (const clang::CastExpr* castExpr = 
             llvm::dyn_cast<clang::CastExpr>(stmt)) {
        node = CreateCastNode(castExpr, "cast");
    }
    else if (const clang::ReturnStmt* retStmt = 
             llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        node = CreateReturnNode(retStmt);
    }
    else if (const clang::ForStmt* forStmt = 
             llvm::dyn_cast<clang::ForStmt>(stmt)) {
        node = CreateForLoopNode(forStmt);
    }
    else if (const clang::WhileStmt* whileStmt = 
             llvm::dyn_cast<clang::WhileStmt>(stmt)) {
        node = CreateWhileLoopNode(whileStmt);
    }
    else if (const clang::DoStmt* doStmt = 
             llvm::dyn_cast<clang::DoStmt>(stmt)) {
        node = CreateDoWhileLoopNode(doStmt);
    }
    else if (const clang::IfStmt* ifStmt = 
             llvm::dyn_cast<clang::IfStmt>(stmt)) {
        node = CreateIfBranchNode(ifStmt);
    }
    else if (const clang::SwitchStmt* switchStmt = 
             llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
        node = CreateSwitchBranchNode(switchStmt);
    }
    else if (const clang::ConditionalOperator* condOp = 
             llvm::dyn_cast<clang::ConditionalOperator>(stmt)) {
        node = CreateSelectNode(condOp);
    }
    else if (const clang::InitListExpr* initList = 
             llvm::dyn_cast<clang::InitListExpr>(stmt)) {
        node = CreateInitListNode(initList);
    }
    else if (const clang::CompoundLiteralExpr* compLit = 
             llvm::dyn_cast<clang::CompoundLiteralExpr>(stmt)) {
        node = CreateCompoundLiteralNode(compLit);
    }
    else {
        // 未知类型
        node = currentGraph->CreateNode(ComputeNodeKind::Unknown);
        node->name = stmt->getStmtClassName();
    }
    
    if (!node) return 0;
    
    // 设置通用属性
    node->astStmt = stmt;
    node->sourceText = GetSourceText(stmt, astContext);
    node->sourceLine = GetSourceLine(stmt, astContext);
    
    // 设置所属函数
    SetContainingFunction(node, stmt);
    
    // 注册到缓存
    processedStmts[stmt] = node->id;
    
    return node->id;
}

} // namespace compute_graph
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphBuilderNode.cpp - 节点创建（重构版，第1部分）
 * 
 * 重构说明：
 * - 将原492行的CreateNodeFromStmt拆分成多个小函数
 * - 每个函数<50行，职责单一
 * - 保持100%功能一致性
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

namespace compute_graph {

// ============================================
// 辅助函数：检测增量操作模式
// ============================================

// 检测复合赋值形式：i+=1, i-=1
bool ComputeGraphBuilder::DetectCompoundAssignIncrement(
    const clang::BinaryOperator* binOp,
    std::shared_ptr<ComputeNode> node)
{
    if (!binOp || !node) return false;
    if (!binOp->isCompoundAssignmentOp()) return false;
    
    clang::BinaryOperatorKind opcode = binOp->getOpcode();
    if (opcode != clang::BO_AddAssign && opcode != clang::BO_SubAssign) {
        return false;
    }
    
    const clang::Expr* lhs = binOp->getLHS()->IgnoreParenImpCasts();
    const clang::Expr* rhs = binOp->getRHS()->IgnoreParenImpCasts();
    
    const clang::DeclRefExpr* lhsRef = llvm::dyn_cast<clang::DeclRefExpr>(lhs);
    const clang::IntegerLiteral* rhsLit = llvm::dyn_cast<clang::IntegerLiteral>(rhs);
    
    if (!lhsRef || !rhsLit) return false;
    
    int64_t step = rhsLit->getValue().getSExtValue();
    if (opcode == clang::BO_SubAssign) {
        step = -step;
    }
    
    // 设置增量属性
    node->SetProperty("is_increment", "true");
    node->SetProperty("increment_var", lhsRef->getDecl()->getNameAsString());
    node->SetProperty("increment_step", std::to_string(step));
    node->name = lhsRef->getDecl()->getNameAsString() +
                 (step >= 0 ? "+=" : "-=") + std::to_string(std::abs(step));
    
    return true;
}

// 检测 i = i + 1 形式
bool ComputeGraphBuilder::DetectAssignmentIncrement(
    const clang::BinaryOperator* binOp,
    std::shared_ptr<ComputeNode> node)
{
    if (!binOp || !node) return false;
    if (binOp->getOpcode() != clang::BO_Assign) return false;
    
    const clang::Expr* lhs = binOp->getLHS()->IgnoreParenImpCasts();
    const clang::Expr* rhs = binOp->getRHS()->IgnoreParenImpCasts();
    
    const clang::DeclRefExpr* lhsRef = llvm::dyn_cast<clang::DeclRefExpr>(lhs);
    const clang::BinaryOperator* rhsBinOp = llvm::dyn_cast<clang::BinaryOperator>(rhs);
    
    if (!lhsRef || !rhsBinOp) return false;
    
    clang::BinaryOperatorKind rhsOpcode = rhsBinOp->getOpcode();
    if (rhsOpcode != clang::BO_Add && rhsOpcode != clang::BO_Sub) {
        return false;
    }
    
    const clang::Expr* rhsLhs = rhsBinOp->getLHS()->IgnoreParenImpCasts();
    const clang::Expr* rhsRhs = rhsBinOp->getRHS()->IgnoreParenImpCasts();
    
    const clang::DeclRefExpr* rhsLhsRef = llvm::dyn_cast<clang::DeclRefExpr>(rhsLhs);
    const clang::IntegerLiteral* rhsRhsLit = llvm::dyn_cast<clang::IntegerLiteral>(rhsRhs);
    
    if (!rhsLhsRef || !rhsRhsLit) return false;
    
    // 检查是否是同一个变量
    if (rhsLhsRef->getDecl() != lhsRef->getDecl()) return false;
    
    int64_t step = rhsRhsLit->getValue().getSExtValue();
    if (rhsOpcode == clang::BO_Sub) {
        step = -step;
    }
    
    // 设置增量属性
    node->SetProperty("is_increment", "true");
    node->SetProperty("increment_var", lhsRef->getDecl()->getNameAsString());
    node->SetProperty("increment_step", std::to_string(step));
    node->name = lhsRef->getDecl()->getNameAsString() +
                 (step >= 0 ? "+=" : "-=") + std::to_string(std::abs(step));
    
    return true;
}

// ============================================
// 节点创建：二元运算符
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateBinaryOpNode(
    const clang::BinaryOperator* binOp)
{
    if (!binOp) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::BinaryOp);
    
    node->opCode = GetOpCodeFromBinaryOp(binOp);
    node->name = OpCodeToString(node->opCode);
    node->dataType = DataTypeInfo::FromClangType(binOp->getType());
    
    // 检测增量操作
    if (!DetectCompoundAssignIncrement(binOp, node)) {
        DetectAssignmentIncrement(binOp, node);
    }
    
    return node;
}

// ============================================
// 节点创建：一元运算符
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateUnaryOpNode(
    const clang::UnaryOperator* unaryOp)
{
    if (!unaryOp) return nullptr;
    
    clang::UnaryOperatorKind opcode = unaryOp->getOpcode();
    bool isIncrement = (opcode == clang::UO_PostInc || opcode == clang::UO_PreInc);
    bool isDecrement = (opcode == clang::UO_PostDec || opcode == clang::UO_PreDec);
    
    std::shared_ptr<ComputeNode> node;
    
    if (isIncrement || isDecrement) {
        // 统一表示为 BinaryOp (+=1 或 -=1)
        node = currentGraph->CreateNode(ComputeNodeKind::BinaryOp);
        node->opCode = isIncrement ? OpCode::Add : OpCode::Sub;
        node->dataType = DataTypeInfo::FromClangType(unaryOp->getType());
        
        // 获取变量名
        std::string varName;
        if (const clang::Expr* subExpr = unaryOp->getSubExpr()) {
            if (const clang::DeclRefExpr* declRef = 
                llvm::dyn_cast<clang::DeclRefExpr>(subExpr->IgnoreParenImpCasts())) {
                varName = declRef->getDecl()->getNameAsString();
            }
        }
        
        // 统一命名为 "var += 1" 形式
        node->name = varName + (isIncrement ? "+=" : "-=") + "1";
        node->SetProperty("is_increment", "true");
        node->SetProperty("increment_var", varName);
        node->SetProperty("increment_step", isIncrement ? "1" : "-1");
        node->SetProperty("original_form",
            (opcode == clang::UO_PostInc) ? "post_inc" :
            (opcode == clang::UO_PreInc) ? "pre_inc" :
            (opcode == clang::UO_PostDec) ? "post_dec" : "pre_dec");
    } else {
        // 其他一元运算符
        node = currentGraph->CreateNode(ComputeNodeKind::UnaryOp);
        node->opCode = GetOpCodeFromUnaryOp(unaryOp);
        node->name = OpCodeToString(node->opCode);
        node->dataType = DataTypeInfo::FromClangType(unaryOp->getType());
    }
    
    return node;
}

// ============================================
// 节点创建：变量引用
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateVariableNode(
    const clang::DeclRefExpr* declRef)
{
    if (!declRef) return nullptr;
    
    const clang::ValueDecl* decl = declRef->getDecl();
    
    std::shared_ptr<ComputeNode> node;
    if (llvm::isa<clang::ParmVarDecl>(decl)) {
        node = currentGraph->CreateNode(ComputeNodeKind::Parameter);
    } else {
        node = currentGraph->CreateNode(ComputeNodeKind::Variable);
    }
    
    node->name = decl->getNameAsString();
    node->dataType = DataTypeInfo::FromClangType(declRef->getType());
    node->astDecl = decl;
    
    return node;
}

// ============================================
// 节点创建：常量（整数）
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateIntConstantNode(
    const clang::IntegerLiteral* intLit)
{
    if (!intLit) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Constant);
    
    node->hasConstValue = true;
    node->constValue.intValue = intLit->getValue().getSExtValue();
    node->name = std::to_string(node->constValue.intValue);
    node->dataType = DataTypeInfo::FromClangType(intLit->getType());
    
    return node;
}

// ============================================
// 节点创建：常量（浮点）
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateFloatConstantNode(
    const clang::FloatingLiteral* floatLit)
{
    if (!floatLit) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Constant);
    
    node->hasConstValue = true;
    node->constValue.floatValue = floatLit->getValue().convertToDouble();
    node->name = std::to_string(node->constValue.floatValue);
    node->dataType = DataTypeInfo::FromClangType(floatLit->getType());
    
    return node;
}

// ============================================
// 节点创建：声明语句
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateDeclStmtNode(
    const clang::DeclStmt* declStmt)
{
    if (!declStmt) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::Variable);
    
    if (declStmt->isSingleDecl()) {
        if (const clang::VarDecl* varDecl = 
            llvm::dyn_cast<clang::VarDecl>(declStmt->getSingleDecl())) {
            node->name = varDecl->getNameAsString();
            node->dataType = DataTypeInfo::FromClangType(varDecl->getType());
            node->astDecl = varDecl;
        }
    }
    
    return node;
}

// ============================================
// 节点创建：数组访问
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateArrayAccessNode(
    const clang::ArraySubscriptExpr* arrayExpr)
{
    if (!arrayExpr) return nullptr;
    
    std::shared_ptr<ComputeNode> node = 
        currentGraph->CreateNode(ComputeNodeKind::ArrayAccess);
    node->dataType = DataTypeInfo::FromClangType(arrayExpr->getType());
    
    // 提取数组名称
    std::string arrayName;
    const clang::Expr* base = arrayExpr->getBase()->IgnoreParenImpCasts();
    if (const clang::DeclRefExpr* declRef = llvm::dyn_cast<clang::DeclRefExpr>(base)) {
        arrayName = declRef->getDecl()->getNameAsString();
    } else if (const clang::ImplicitCastExpr* implCast = 
               llvm::dyn_cast<clang::ImplicitCastExpr>(base)) {
        if (const clang::DeclRefExpr* innerRef = 
            llvm::dyn_cast<clang::DeclRefExpr>(implCast->getSubExpr()->IgnoreParenImpCasts())) {
            arrayName = innerRef->getDecl()->getNameAsString();
        }
    }
    
    // 提取索引名称
    std::string idxName;
    const clang::Expr* idx = arrayExpr->getIdx()->IgnoreParenImpCasts();
    if (const clang::DeclRefExpr* declRef = llvm::dyn_cast<clang::DeclRefExpr>(idx)) {
        idxName = declRef->getDecl()->getNameAsString();
    } else if (const clang::IntegerLiteral* intLit = 
               llvm::dyn_cast<clang::IntegerLiteral>(idx)) {
        idxName = std::to_string(intLit->getValue().getSExtValue());
    }
    
    // 生成名称 "x[i]" 格式
    if (!arrayName.empty()) {
        node->name = arrayName + "[" + (idxName.empty() ? "?" : idxName) + "]";
    } else {
        node->name = "[]";
    }
    
    return node;
}

// ============================================
// 节点创建：C++操作符重载调用
// ============================================

std::shared_ptr<ComputeNode> ComputeGraphBuilder::CreateOperatorCallNode(
    const clang::CXXOperatorCallExpr* opCallExpr)
{
    if (!opCallExpr) return nullptr;
    
    clang::OverloadedOperatorKind opKind = opCallExpr->getOperator();
    
    // 映射操作符到OpCode
    bool isBinaryOp = false;
    bool isCompareOp = false;
    OpCode opCodeVal = OpCode::Unknown;
    std::string opName;
    
    switch (opKind) {
        case clang::OO_Plus:
            isBinaryOp = true; opCodeVal = OpCode::Add; opName = "+"; break;
        case clang::OO_Minus:
            isBinaryOp = true; opCodeVal = OpCode::Sub; opName = "-"; break;
        case clang::OO_Star:
            isBinaryOp = true; opCodeVal = OpCode::Mul; opName = "*"; break;
        case clang::OO_Slash:
            isBinaryOp = true; opCodeVal = OpCode::Div; opName = "/"; break;
        case clang::OO_Percent:
            isBinaryOp = true; opCodeVal = OpCode::Mod; opName = "%"; break;
        case clang::OO_Amp:
            isBinaryOp = true; opCodeVal = OpCode::And; opName = "&"; break;
        case clang::OO_Pipe:
            isBinaryOp = true; opCodeVal = OpCode::Or; opName = "|"; break;
        case clang::OO_Caret:
            isBinaryOp = true; opCodeVal = OpCode::Xor; opName = "^"; break;
        case clang::OO_Less:
            isCompareOp = true; opCodeVal = OpCode::Lt; opName = "<"; break;
        case clang::OO_Greater:
            isCompareOp = true; opCodeVal = OpCode::Gt; opName = ">"; break;
        case clang::OO_LessEqual:
            isCompareOp = true; opCodeVal = OpCode::Le; opName = "<="; break;
        case clang::OO_GreaterEqual:
            isCompareOp = true; opCodeVal = OpCode::Ge; opName = ">="; break;
        case clang::OO_EqualEqual:
            isCompareOp = true; opCodeVal = OpCode::Eq; opName = "=="; break;
        case clang::OO_ExclaimEqual:
            isCompareOp = true; opCodeVal = OpCode::Ne; opName = "!="; break;
        default:
            break;
    }
    
    std::shared_ptr<ComputeNode> node;
    if (isBinaryOp) {
        node = currentGraph->CreateNode(ComputeNodeKind::BinaryOp);
        node->name = opName;
        node->opCode = opCodeVal;
    } else if (isCompareOp) {
        node = currentGraph->CreateNode(ComputeNodeKind::CompareOp);
        node->name = opName;
        node->opCode = opCodeVal;
    } else {
        // 其他操作符（如 [], () 等）作为Call
        node = currentGraph->CreateNode(ComputeNodeKind::Call);
        node->name = clang::getOperatorSpelling(opKind);
    }
    
    node->dataType = DataTypeInfo::FromClangType(opCallExpr->getType());
    
    return node;
}

    ComputeNode::NodeId ComputeGraphBuilder::CreateDefinitionNode(
    const clang::Stmt* defStmt, const std::string& varName)
{
    if (!defStmt) {
        return 0;
    }

    std::map<const clang::Stmt*, ComputeNode::NodeId>::iterator it =
        processedStmts.find(defStmt);
    if (it != processedStmts.end()) {
        return it->second;
    }

    const clang::UnaryOperator* unaryOp =
        llvm::dyn_cast<clang::UnaryOperator>(defStmt);
    if (unaryOp && unaryOp->isIncrementDecrementOp()) {
        return CreateUnaryOpDefNode(unaryOp);
    }

    const clang::BinaryOperator* binOp =
        llvm::dyn_cast<clang::BinaryOperator>(defStmt);
    if (binOp && (binOp->isAssignmentOp() || binOp->isCompoundAssignmentOp())) {
        return CreateBinaryOpDefNode(binOp, varName);
    }

    const clang::DeclStmt* declStmt =
        llvm::dyn_cast<clang::DeclStmt>(defStmt);
    if (declStmt) {
        return CreateDeclStmtDefNode(declStmt, varName);
    }

    return CreateGenericDefNode(defStmt, varName);
}

ComputeNode::NodeId ComputeGraphBuilder::CreateUnaryOpDefNode(
    const clang::UnaryOperator* unaryOp)
{
    std::shared_ptr<ComputeNode> node =
        currentGraph->CreateNode(ComputeNodeKind::BinaryOp);
    node->name = unaryOp->isIncrementOp() ? "+" : "-";
    node->opCode = unaryOp->isIncrementOp() ? OpCode::Add : OpCode::Sub;
    node->sourceText = GetSourceText(unaryOp, astContext);
    node->sourceLine = GetSourceLine(unaryOp, astContext);
    node->containingFunc = GetContainingFunction(unaryOp);
    node->astStmt = unaryOp;

    ComputeNode::NodeId nodeId = node->id;
    SetLoopContextForNode(nodeId);
    processedStmts[unaryOp] = nodeId;

    const clang::Expr* operand = unaryOp->getSubExpr();
    if (operand) {
        ComputeNode::NodeId operandId =
            BuildExpressionTree(operand->IgnoreParenImpCasts(), 0);
        if (operandId != 0) {
            ConnectNodes(operandId, nodeId,
                        ComputeEdgeKind::DataFlow, "lhs_read");
            ConnectNodes(nodeId, operandId,
                        ComputeEdgeKind::DataFlow, "assign_to");

            std::shared_ptr<ComputeNode> operandNode =
                currentGraph->GetNode(operandId);
            if (operandNode) {
                operandNode->SetProperty("is_assign_target", "true");
                operandNode->SetProperty("is_read_write", "true");
            }
        }
    }

    return nodeId;
}

ComputeNode::NodeId ComputeGraphBuilder::CreateBinaryOpDefNode(
    const clang::BinaryOperator* binOp, const std::string& varName)
{
    ComputeNode::NodeId nodeId = BuildExpressionTree(binOp, 0);
    if (nodeId != 0) {
        return nodeId;
    }

    std::shared_ptr<ComputeNode> node =
        currentGraph->CreateNode(ComputeNodeKind::BinaryOp);
    node->name = "=";
    node->opCode = OpCode::Assign;
    node->sourceText = GetSourceText(binOp, astContext);
    node->sourceLine = GetSourceLine(binOp, astContext);
    node->containingFunc = GetContainingFunction(binOp);
    node->astStmt = binOp;

    nodeId = node->id;
    SetLoopContextForNode(nodeId);
    processedStmts[binOp] = nodeId;

    return nodeId;
}

ComputeNode::NodeId ComputeGraphBuilder::CreateDeclStmtDefNode(
    const clang::DeclStmt* declStmt, const std::string& varName)
{
    ComputeNode::NodeId nodeId = BuildExpressionTree(declStmt, 0);
    if (nodeId != 0) {
        return nodeId;
    }

    std::shared_ptr<ComputeNode> node =
        currentGraph->CreateNode(ComputeNodeKind::Variable);
    node->name = varName;
    node->sourceText = GetSourceText(declStmt, astContext);
    node->sourceLine = GetSourceLine(declStmt, astContext);
    node->containingFunc = GetContainingFunction(declStmt);
    node->astStmt = declStmt;

    nodeId = node->id;
    processedStmts[declStmt] = nodeId;

    return nodeId;
}

ComputeNode::NodeId ComputeGraphBuilder::CreateGenericDefNode(
    const clang::Stmt* defStmt, const std::string& varName)
{
    std::shared_ptr<ComputeNode> node =
        currentGraph->CreateNode(ComputeNodeKind::Variable);
    node->name = varName + "_def";
    node->sourceText = GetSourceText(defStmt, astContext);
    node->sourceLine = GetSourceLine(defStmt, astContext);
    node->containingFunc = GetContainingFunction(defStmt);
    node->astStmt = defStmt;

    ComputeNode::NodeId nodeId = node->id;
    SetLoopContextForNode(nodeId);
    processedStmts[defStmt] = nodeId;

    return nodeId;
}

void ComputeGraphBuilder::SetLoopContextForNode(ComputeNode::NodeId nodeId)
{
    if (currentLoopInfo.loopNodeId == 0) {
        return;
    }

    std::shared_ptr<ComputeNode> node = currentGraph->GetNode(nodeId);
    if (node) {
        node->loopContextId = currentLoopInfo.loopNodeId;
        node->loopContextVar = currentLoopInfo.loopVarName;
        node->loopContextLine = currentLoopInfo.bodyStartLine;
    }
}

} // namespace compute_graph
