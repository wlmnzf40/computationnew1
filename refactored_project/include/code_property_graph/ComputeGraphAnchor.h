#ifndef COMPUTEGRAPHREFACTORED_COMPUTEGRAPHANCHOR_H
#define COMPUTEGRAPHREFACTORED_COMPUTEGRAPHANCHOR_H

#include "CPGAnnotation.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include <set>
#include <vector>
#include "ComputeGraphBase.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"
#include "clang/Lex/Lexer.h"
#include <algorithm>

namespace compute_graph {
    // ============================================
    // 锚点信息 (向量化起点)
    // ============================================
    struct AnchorPoint {
        const clang::Stmt* stmt = nullptr;
        const clang::FunctionDecl* func = nullptr;
        ComputeNodeKind expectedKind = ComputeNodeKind::Unknown;
        OpCode opCode = OpCode::Unknown;
        int loopDepth = 0;
        bool isInLoop = false;

        // 锚点评分 (用于优先级排序)
        int score = 0;

        // 源码信息
        std::string sourceText;
        int sourceLine = 0;

        std::string ToString() const;
    };


    // ============================================
    // 锚点查找器
    // ============================================
    class AnchorFinder {
    public:
        explicit AnchorFinder(cpg::CPGContext& cpgCtx, clang::ASTContext& astCtx);

        // 查找所有潜在的向量化锚点
        std::vector<AnchorPoint> FindAllAnchors();

        // 查找特定函数中的锚点
        std::vector<AnchorPoint> FindAnchorsInFunction(const clang::FunctionDecl* func);

        // 过滤和排序锚点
        std::vector<AnchorPoint> FilterAndRankAnchors(
            const std::vector<AnchorPoint>& anchors);

        // 配置
        void SetMinLoopDepth(int depth) { minLoopDepth = depth; }
        void SetIncludeNonLoopOps(bool include) { includeNonLoopOps = include; }

    private:
        cpg::CPGContext& cpgContext;
        clang::ASTContext& astContext;

        int minLoopDepth = 0;       // 最小循环深度要求
        bool includeNonLoopOps = true;  // 是否包含非循环内的运算

        // 判断语句是否是潜在的向量化锚点
        bool IsVectorizableAnchor(const clang::Stmt* stmt, int loopDepth) const;

        // 计算锚点评分
        int ComputeAnchorScore(const AnchorPoint& anchor) const;

        // 获取语句的循环深度
        int GetLoopDepth(const clang::Stmt* stmt) const;

        // 判断是否在循环内
        bool IsInLoop(const clang::Stmt* stmt) const;
    };


// ============================================
// AnchorVisitor - 锚点查找访问器
// ============================================
class AnchorVisitor : public clang::RecursiveASTVisitor<AnchorVisitor> {
public:
    AnchorFinder& finder;
    const clang::FunctionDecl* currentFunc;
    std::vector<AnchorPoint>& anchors;
    clang::ASTContext& astContext;
    int currentLoopDepth;
    bool isInLoopIncrement;
    std::set<const clang::Stmt*> addedStmts;

    // 构造函数
    AnchorVisitor(AnchorFinder& f, const clang::FunctionDecl* func,
                  std::vector<AnchorPoint>& a, clang::ASTContext& ctx);

    // 遍历方法
    bool TraverseForStmt(clang::ForStmt* stmt);
    bool TraverseWhileStmt(clang::WhileStmt* stmt);
    bool TraverseDoStmt(clang::DoStmt* stmt);
    bool VisitBinaryOperator(clang::BinaryOperator* binOp);

private:
    // 核心处理方法
    bool ProcessAssignment(clang::BinaryOperator* binOp);
    bool ProcessNonAssignment(clang::BinaryOperator* binOp);
    bool CheckTopLevelExpression(clang::BinaryOperator* binOp);

    // 辅助方法
    void AddAnchor(clang::Stmt* stmt, ComputeNodeKind kind, OpCode opCode);
    void MarkSubExprsAsAdded(clang::Stmt* stmt);

    // 位置检查方法
    bool IsInArraySubscript(const clang::Expr* expr);
    bool IsDescendantOf(const clang::Expr* expr, const clang::Expr* ancestor);
    bool IsSimpleArrayIndexExpr(const clang::BinaryOperator* binOp);
    bool IsInLoopCondition(const clang::Expr* expr);
    bool IsInIfCondition(const clang::Expr* expr);

    // 操作类型检查
    bool IsVectorizableBinaryOp(const clang::BinaryOperator* op);
    bool IsComparisonOp(const clang::BinaryOperator* op);
    bool ContainsArrayAccess(const clang::Expr* expr);
    bool ContainsVectorizableOp(const clang::Expr* expr);

    // 其他辅助方法
    int CountOperations(const clang::Expr* expr);
    OpCode GetOpCode(const clang::BinaryOperator* op);
};
}
#endif //COMPUTEGRAPHREFACTORED_COMPUTEGRAPHANCHOR_H