
#include "ComputeGraphAnchor.h"
#include "CPGAnnotation.h"
namespace compute_graph {
    // ============================================
    // AnchorFinder 实现
    // ============================================

    AnchorFinder::AnchorFinder(cpg::CPGContext& cpgCtx, clang::ASTContext& astCtx)
        : cpgContext(cpgCtx), astContext(astCtx)
    {
        // cpgContext 保留供未来跨函数分析使用
        (void)cpgContext;
    }

    std::vector<AnchorPoint> AnchorFinder::FindAllAnchors()
    {
        std::vector<AnchorPoint> allAnchors;
        clang::SourceManager& sm = astContext.getSourceManager();

        for (auto* decl : astContext.getTranslationUnitDecl()->decls()) {
            // 跳过系统头文件中的声明
            if (decl->getLocation().isValid() &&
                sm.isInSystemHeader(decl->getLocation())) {
                continue;
                }

            // 获取要处理的函数声明
            const clang::FunctionDecl* func = nullptr;

            // 普通函数
            if (auto* funcDecl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                func = funcDecl;
            }
            // 模板函数 - 获取模板的定义
            else if (auto* funcTemplate = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
                func = funcTemplate->getTemplatedDecl();
            }

            if (func && func->hasBody() && func->isThisDeclarationADefinition()) {
                // 再次检查函数体的位置
                if (func->getBody()->getBeginLoc().isValid() &&
                    sm.isInSystemHeader(func->getBody()->getBeginLoc())) {
                    continue;
                    }
                auto funcAnchors = FindAnchorsInFunction(func);
                allAnchors.insert(allAnchors.end(),
                                  funcAnchors.begin(), funcAnchors.end());
            }
        }

        return allAnchors;
    }

    std::vector<AnchorPoint> AnchorFinder::FindAnchorsInFunction(
        const clang::FunctionDecl* func)
    {
        std::vector<AnchorPoint> anchors;
        if (!func || !func->hasBody()) return anchors;

        AnchorVisitor visitor(*this, func, anchors, astContext);
        visitor.TraverseStmt(func->getBody());

        for (auto& anchor : anchors) {
            anchor.score = ComputeAnchorScore(anchor);
        }

        return anchors;
    }

    std::vector<AnchorPoint> AnchorFinder::FilterAndRankAnchors(
        const std::vector<AnchorPoint>& anchors)
    {
        // 【新增】按语句去重（第一层去重）
        std::set<const clang::Stmt*> seenStmts;
        std::vector<AnchorPoint> uniqueAnchors;

        for (const auto& anchor : anchors) {
            if (seenStmts.count(anchor.stmt)) {
                continue;  // 跳过重复语句
            }
            seenStmts.insert(anchor.stmt);
            uniqueAnchors.push_back(anchor);
        }

        // 【修改】多级去重：语句指针 + 源码位置（第二层去重）
        std::set<const clang::Stmt*> seenStmts2;
        std::set<std::string> seenLocations;  // "funcName:line"
        std::vector<AnchorPoint> filtered;

        for (const auto& anchor : uniqueAnchors) {
            // 1. 基于语句指针去重
            if (seenStmts2.count(anchor.stmt)) continue;

            // 2. 基于源码位置去重（同一行的同类操作）
            std::string locKey = (anchor.func ? anchor.func->getNameAsString() : "unknown");
            locKey += ":" + std::to_string(anchor.sourceLine);
            if (seenLocations.count(locKey)) continue;

            // 3. 过滤非循环操作（如果配置要求）
            if (anchor.loopDepth < minLoopDepth && !includeNonLoopOps) {
                continue;
            }

            filtered.push_back(anchor);
            seenStmts2.insert(anchor.stmt);
            seenLocations.insert(locKey);
        }

        // 【修改】对去重后的锚点进行排序
        std::sort(filtered.begin(), filtered.end(),
            [this](const AnchorPoint& a, const AnchorPoint& b) {
                return ComputeAnchorScore(a) > ComputeAnchorScore(b);
            });

        // 【新增】限制锚点数量，避免过多重复分析
        const size_t MAX_ANCHORS = 50;
        if (filtered.size() > MAX_ANCHORS) {
            llvm::outs() << "  [FilterAnchors] Limiting from " << filtered.size()
                         << " to " << MAX_ANCHORS << " anchors\n";
            filtered.resize(MAX_ANCHORS);
        }

        return filtered;
    }


    int AnchorFinder::ComputeAnchorScore(const AnchorPoint& anchor) const
    {
        int score = 0;
        score += anchor.loopDepth * 100;

        switch (anchor.opCode) {
            case OpCode::Mul:
                score += 80;
                break;
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Shl:
            case OpCode::Shr:
            case OpCode::And:
            case OpCode::Or:
            case OpCode::Xor:
                score += 60;
                break;
            case OpCode::Div:
            case OpCode::Mod:
                score += 40;
                break;
            default:
                break;
        }

        if (anchor.expectedKind == ComputeNodeKind::ArrayAccess) {
            score += 70;
        }
        if (anchor.expectedKind == ComputeNodeKind::Call) {
            score += 50;
        }

        return score;
    }




// ============================================
// 构造函数
// ============================================
AnchorVisitor::AnchorVisitor(
    AnchorFinder& f,
    const clang::FunctionDecl* func,
    std::vector<AnchorPoint>& a,
    clang::ASTContext& ctx)
    : finder(f),
      currentFunc(func),
      anchors(a),
      astContext(ctx),
      currentLoopDepth(0),
      isInLoopIncrement(false)
{
}

// ============================================
// 遍历方法实现
// ============================================
bool AnchorVisitor::TraverseForStmt(clang::ForStmt* stmt)
{
    if (!stmt) {
        return true;
    }

    currentLoopDepth++;

    if (stmt->getInit()) {
        TraverseStmt(stmt->getInit());
    }

    if (stmt->getCond()) {
        TraverseStmt(stmt->getCond());
    }

    if (stmt->getInc()) {
        bool oldFlag = isInLoopIncrement;
        isInLoopIncrement = true;
        TraverseStmt(stmt->getInc());
        isInLoopIncrement = oldFlag;
    }

    if (stmt->getBody()) {
        TraverseStmt(stmt->getBody());
    }

    currentLoopDepth--;
    return true;
}

bool AnchorVisitor::TraverseWhileStmt(clang::WhileStmt* stmt)
{
    currentLoopDepth++;
    bool result = clang::RecursiveASTVisitor<AnchorVisitor>::
        TraverseWhileStmt(stmt);
    currentLoopDepth--;
    return result;
}

bool AnchorVisitor::TraverseDoStmt(clang::DoStmt* stmt)
{
    currentLoopDepth++;
    bool result = clang::RecursiveASTVisitor<AnchorVisitor>::
        TraverseDoStmt(stmt);
    currentLoopDepth--;
    return result;
}

// ============================================
// VisitBinaryOperator - 主入口（已拆分）
// ============================================
bool AnchorVisitor::VisitBinaryOperator(clang::BinaryOperator* binOp)
{
    if (isInLoopIncrement) {
        return true;
    }

    if (binOp->getOpcode() == clang::BO_Assign) {
        return ProcessAssignment(binOp);
    }

    return ProcessNonAssignment(binOp);
}

// ============================================
// 赋值操作处理
// ============================================
bool AnchorVisitor::ProcessAssignment(clang::BinaryOperator* binOp)
{
    clang::Expr* rhs = binOp->getRHS()->IgnoreParenImpCasts();

    if (ContainsVectorizableOp(rhs)) {
        AddAnchor(binOp, ComputeNodeKind::BinaryOp, OpCode::Assign);
        MarkSubExprsAsAdded(binOp);
        return true;
    }

    if (ContainsArrayAccess(rhs)) {
        clang::Expr* lhs = binOp->getLHS()->IgnoreParenImpCasts();
        if (ContainsArrayAccess(lhs)) {
            AddAnchor(binOp, ComputeNodeKind::BinaryOp, OpCode::Assign);
            MarkSubExprsAsAdded(binOp);
            return true;
        }
    }

    return true;
}

// ============================================
// 非赋值操作处理
// ============================================
bool AnchorVisitor::ProcessNonAssignment(clang::BinaryOperator* binOp)
{
    if (IsInLoopCondition(binOp)) {
        return true;
    }

    bool inIfCondition = IsInIfCondition(binOp);
    bool isComparison = IsComparisonOp(binOp);

    if (inIfCondition && !isComparison) {
        return true;
    }

    if (!IsVectorizableBinaryOp(binOp)) {
        return true;
    }

    if (addedStmts.count(binOp)) {
        return true;
    }

    if (IsSimpleArrayIndexExpr(binOp)) {
        return true;
    }

    return CheckTopLevelExpression(binOp);
}

// ============================================
// 检查顶层表达式
// ============================================
bool AnchorVisitor::CheckTopLevelExpression(clang::BinaryOperator* binOp)
{
    clang::DynTypedNodeList parents =
        astContext.getParents(*binOp);

    bool hasParentBinOp = false;

    for (const clang::DynTypedNode& parent : parents) {
        const clang::BinaryOperator* parentBinOp =
            parent.get<clang::BinaryOperator>();

        if (parentBinOp && IsVectorizableBinaryOp(parentBinOp)) {
            hasParentBinOp = true;
            break;
        }
    }

    if (!hasParentBinOp) {
        AddAnchor(binOp, ComputeNodeKind::BinaryOp, GetOpCode(binOp));
        MarkSubExprsAsAdded(binOp);
    }

    return true;
}

// ============================================
// 锚点管理方法
// ============================================
void AnchorVisitor::AddAnchor(
    clang::Stmt* stmt,
    ComputeNodeKind kind,
    OpCode opCode)
{
    if (addedStmts.count(stmt)) {
        return;
    }

    AnchorPoint anchor;
    anchor.stmt = stmt;
    anchor.func = currentFunc;
    anchor.expectedKind = kind;
    anchor.opCode = opCode;
    anchor.loopDepth = currentLoopDepth;
    anchor.isInLoop = (currentLoopDepth > 0);
    anchor.sourceText = GetSourceText(stmt, astContext);
    anchor.sourceLine = GetSourceLine(stmt, astContext);

    anchors.push_back(anchor);
    addedStmts.insert(stmt);
}

void AnchorVisitor::MarkSubExprsAsAdded(clang::Stmt* stmt)
{
    if (!stmt) {
        return;
    }

    addedStmts.insert(stmt);

    for (clang::Stmt* child : stmt->children()) {
        if (child) {
            MarkSubExprsAsAdded(child);
        }
    }
}

// ============================================
// 位置检查方法
// ============================================
bool AnchorVisitor::IsInArraySubscript(const clang::Expr* expr)
{
    clang::DynTypedNodeList parents =
        astContext.getParents(*expr);

    while (!parents.empty()) {
        const clang::DynTypedNode& parent = parents[0];
        const clang::ArraySubscriptExpr* arrayExpr =
            parent.get<clang::ArraySubscriptExpr>();

        if (arrayExpr) {
            const clang::Expr* idx = arrayExpr->getIdx()->IgnoreParenImpCasts();
            if (IsDescendantOf(expr, idx)) {
                return true;
            }
        }

        parents = astContext.getParents(parent);
    }

    return false;
}

bool AnchorVisitor::IsDescendantOf(
    const clang::Expr* expr,
    const clang::Expr* ancestor)
{
    if (expr == ancestor) {
        return true;
    }

    for (const clang::Stmt* child : ancestor->children()) {
        const clang::Expr* childExpr =
            llvm::dyn_cast_or_null<clang::Expr>(child);

        if (childExpr) {
            const clang::Expr* childIgnored =
                childExpr->IgnoreParenImpCasts();

            if (IsDescendantOf(expr, childIgnored)) {
                return true;
            }
        }
    }

    return false;
}

int AnchorVisitor::CountOperations(const clang::Expr* expr)
{
    int count = 0;

    if (llvm::isa<clang::BinaryOperator>(expr)) {
        count = 1;
    }

    for (const clang::Stmt* child : expr->children()) {
        const clang::Expr* childExpr =
            llvm::dyn_cast_or_null<clang::Expr>(child);

        if (childExpr) {
            count += CountOperations(childExpr);
        }
    }

    return count;
}

bool AnchorVisitor::IsSimpleArrayIndexExpr(
    const clang::BinaryOperator* binOp)
{
    if (!IsInArraySubscript(binOp)) {
        return false;
    }

    int complexity = CountOperations(binOp);
    return complexity <= 1;
}

bool AnchorVisitor::IsInLoopCondition(const clang::Expr* expr)
{
    clang::DynTypedNodeList parents =
        astContext.getParents(*expr);

    while (!parents.empty()) {
        const clang::DynTypedNode& parent = parents[0];

        const clang::ForStmt* forStmt = parent.get<clang::ForStmt>();
        if (forStmt) {
            const clang::Expr* cond = forStmt->getCond();
            if (cond && IsDescendantOf(expr, cond)) {
                return true;
            }
        }

        const clang::WhileStmt* whileStmt = parent.get<clang::WhileStmt>();
        if (whileStmt) {
            const clang::Expr* cond = whileStmt->getCond();
            if (cond && IsDescendantOf(expr, cond)) {
                return true;
            }
        }

        const clang::DoStmt* doStmt = parent.get<clang::DoStmt>();
        if (doStmt) {
            const clang::Expr* cond = doStmt->getCond();
            if (cond && IsDescendantOf(expr, cond)) {
                return true;
            }
        }

        const clang::Stmt* pStmt = parent.get<clang::Stmt>();
        if (pStmt) {
            parents = astContext.getParents(*pStmt);
        } else {
            break;
        }
    }

    return false;
}

bool AnchorVisitor::IsInIfCondition(const clang::Expr* expr)
{
    clang::DynTypedNodeList parents =
        astContext.getParents(*expr);

    while (!parents.empty()) {
        const clang::DynTypedNode& parent = parents[0];

        const clang::IfStmt* ifStmt = parent.get<clang::IfStmt>();
        if (ifStmt) {
            const clang::Expr* cond = ifStmt->getCond();
            if (cond && IsDescendantOf(expr, cond)) {
                return true;
            }
        }

        const clang::Stmt* pStmt = parent.get<clang::Stmt>();
        if (pStmt) {
            parents = astContext.getParents(*pStmt);
        } else {
            break;
        }
    }

    return false;
}

// ============================================
// 操作类型检查方法
// ============================================
bool AnchorVisitor::IsVectorizableBinaryOp(
    const clang::BinaryOperator* op)
{
    switch (op->getOpcode()) {
        case clang::BO_Add:
        case clang::BO_Sub:
        case clang::BO_Mul:
        case clang::BO_Div:
        case clang::BO_Rem:
        case clang::BO_Shl:
        case clang::BO_Shr:
        case clang::BO_And:
        case clang::BO_Or:
        case clang::BO_Xor:
        case clang::BO_LT:
        case clang::BO_GT:
        case clang::BO_LE:
        case clang::BO_GE:
        case clang::BO_EQ:
        case clang::BO_NE:
        case clang::BO_AddAssign:
        case clang::BO_SubAssign:
        case clang::BO_MulAssign:
        case clang::BO_DivAssign:
        case clang::BO_RemAssign:
        case clang::BO_ShlAssign:
        case clang::BO_ShrAssign:
        case clang::BO_AndAssign:
        case clang::BO_OrAssign:
        case clang::BO_XorAssign:
            return true;
        default:
            return false;
    }
}

bool AnchorVisitor::IsComparisonOp(const clang::BinaryOperator* op)
{
    switch (op->getOpcode()) {
        case clang::BO_LT:
        case clang::BO_GT:
        case clang::BO_LE:
        case clang::BO_GE:
        case clang::BO_EQ:
        case clang::BO_NE:
            return true;
        default:
            return false;
    }
}

bool AnchorVisitor::ContainsArrayAccess(const clang::Expr* expr)
{
    if (!expr) {
        return false;
    }

    if (llvm::isa<clang::ArraySubscriptExpr>(expr)) {
        return true;
    }

    for (const clang::Stmt* child : expr->children()) {
        const clang::Expr* childExpr =
            llvm::dyn_cast_or_null<clang::Expr>(child);

        if (childExpr && ContainsArrayAccess(childExpr)) {
            return true;
        }
    }

    return false;
}

bool AnchorVisitor::ContainsVectorizableOp(const clang::Expr* expr)
{
    if (!expr) {
        return false;
    }

    const clang::BinaryOperator* binOp =
        llvm::dyn_cast<clang::BinaryOperator>(expr);

    if (binOp && IsVectorizableBinaryOp(binOp)) {
        return true;
    }

    const clang::UnaryOperator* unaryOp =
        llvm::dyn_cast<clang::UnaryOperator>(expr);

    if (unaryOp) {
        clang::UnaryOperator::Opcode opcode = unaryOp->getOpcode();

        if (opcode == clang::UO_Minus ||
            opcode == clang::UO_Not ||
            opcode == clang::UO_LNot) {
            return true;
        }
    }

    for (const clang::Stmt* child : expr->children()) {
        const clang::Expr* childExpr =
            llvm::dyn_cast_or_null<clang::Expr>(child);

        if (childExpr && ContainsVectorizableOp(childExpr)) {
            return true;
        }
    }

    return false;
}

// ============================================
// OpCode获取方法
// ============================================
OpCode AnchorVisitor::GetOpCode(const clang::BinaryOperator* op)
{
    switch (op->getOpcode()) {
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
        case clang::BO_RemAssign:
            return OpCode::Mod;
        case clang::BO_Shl:
        case clang::BO_ShlAssign:
            return OpCode::Shl;
        case clang::BO_Shr:
        case clang::BO_ShrAssign:
            return OpCode::Shr;
        case clang::BO_And:
        case clang::BO_AndAssign:
            return OpCode::And;
        case clang::BO_Or:
        case clang::BO_OrAssign:
            return OpCode::Or;
        case clang::BO_Xor:
        case clang::BO_XorAssign:
            return OpCode::Xor;
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
}