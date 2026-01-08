#ifndef COMPUTEGRAPHREFACTORED_COMPUTEGRAPHBASE_H
#define COMPUTEGRAPHREFACTORED_COMPUTEGRAPHBASE_H

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
namespace compute_graph {
// ============================================
// 计算图节点类型
// ============================================
enum class ComputeNodeKind {
    // 基本类型
    Constant,           // 常量节点 (如 2, 3, 3.14)
    Variable,           // 变量节点 (如 a, b, c)
    Parameter,          // 函数参数节点

    // 运算类型
    BinaryOp,           // 二元运算 (+, -, *, /, %, &, |, ^, <<, >>)
    UnaryOp,            // 一元运算 (-, !, ~, ++, --)
    CompareOp,          // 比较运算 (<, >, <=, >=, ==, !=)

    // 内存操作
    Load,               // 内存加载 (数组访问, 指针解引用)
    Store,              // 内存存储 (赋值)
    ArrayAccess,        // 数组访问 arr[i]
    MemberAccess,       // 成员访问 obj.field 或 ptr->field

    // 控制流相关
    Phi,                // SSA Phi节点 (分支汇合点)
    Select,             // 条件选择 (三元运算符)
    LoopInduction,      // 循环归纳变量
    Loop,               // 【新增】循环节点 (for/while/do-while)
    Branch,             // 【新增】分支节点 (if/switch)

    // 函数调用
    Call,               // 普通函数调用
    IntrinsicCall,      // 内置函数/SIMD指令调用

    // 特殊节点
    Cast,               // 类型转换
    Return,             // 返回值
    Unknown             // 未知类型
};

// ============================================
// 计算图边类型
// ============================================
enum class ComputeEdgeKind {
    DataFlow,           // 数据流边 (def-use关系)
    Control,            // 控制依赖边
    Memory,             // 内存依赖边
    Call,               // 函数调用边
    Return,             // 返回值边
    LoopCarried         // 循环携带依赖
};

// ============================================
// 运算符类型 (用于BinaryOp和UnaryOp)
// ============================================
enum class OpCode {
    // 算术运算
    Add, Sub, Mul, Div, Mod,
    // 位运算
    And, Or, Xor, Shl, Shr,
    // 一元运算
    Neg, Not, BitNot,
    // 比较运算
    Lt, Gt, Le, Ge, Eq, Ne,
    // 其他
    Assign, Unknown
};

// ============================================
// 数据类型信息
// ============================================
struct DataTypeInfo {
    enum class BaseType {
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float, Double,
        Pointer, Array, Void,
        TemplateParam,   // 模板参数类型（如 T）
        Dependent,       // 依赖类型（模板中的复杂类型）
        Unknown
    };

    BaseType baseType = BaseType::Unknown;
    int vectorWidth = 1;        // 向量宽度 (1表示标量)
    int bitWidth = 0;           // 位宽
    bool isSigned = true;       // 是否有符号
    std::string typeName;       // 原始类型名（用于模板类型如 "T"）

    std::string ToString() const;
    static DataTypeInfo FromClangType(const clang::QualType& type);
};

    // ============================================
    // StrictUsesFinder - 辅助类定义
    // ============================================
    class StrictUsesFinder : public clang::RecursiveASTVisitor<StrictUsesFinder> {
    public:
        const clang::VarDecl* target;
        int defLine;
        std::vector<const clang::Stmt*> foundUses;
        clang::ASTContext& ctx;

        StrictUsesFinder(const clang::VarDecl* t, int l, clang::ASTContext& c)
            : target(t), defLine(l), ctx(c) {}

        int GetLine(const clang::Stmt* s) {
            return ctx.getSourceManager().getSpellingLineNumber(
                s->getBeginLoc());
        }

        bool VisitDeclRefExpr(clang::DeclRefExpr* ref) {
            if (ref->getDecl() != target) {
                return true;
            }

            foundUses.push_back(ref);
            return true;
        }
    };

    std::string GetSourceText(const clang::Stmt* stmt, clang::ASTContext& ctx);
    int GetSourceLine(const clang::Stmt* stmt, clang::ASTContext& ctx);
    int GetSourceLine(const clang::Decl* decl, clang::ASTContext& ctx);
    bool IsVectorIntrinsicFunction(const clang::FunctionDecl* func,
                                           const clang::SourceManager& sm);
}
#endif //COMPUTEGRAPHREFACTORED_COMPUTEGRAPHBASE_H