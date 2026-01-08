/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef CPPANALYZER_CPGBUILDER_H
#define CPPANALYZER_CPGBUILDER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <queue>

namespace cpg {
// ============================================
// ICFG节点类型
// ============================================
enum class ICFGNodeKind {
    Entry,           // 函数入口
    Exit,            // 函数出口
    Statement,       // 普通语句
    CallSite,        // 调用点
    ReturnSite,      // 返回点
    FormalIn,        // 形参入口
    FormalOut,       // 形参出口
    ActualIn,        // 实参入口
    ActualOut        // 实参出口
};

// ============================================
// ICFG边类型
// ============================================
enum class ICFGEdgeKind {
    Intraprocedural,  // 过程内边
    Call,             // 调用边
    Return,           // 返回边
    ParamIn,          // 参数传入边
    ParamOut,         // 参数传出边
    True,             // true分支
    False,            // false分支
    Unconditional     // 无条件边
};

// ============================================
// ICFG节点
// ============================================
class ICFGNode {
public:
    ICFGNodeKind kind;
    const clang::Stmt* stmt = nullptr;
    const clang::FunctionDecl* func = nullptr;
    const clang::CFGBlock* cfgBlock = nullptr;

    // 调用相关信息
    const clang::CallExpr* callExpr = nullptr;
    const clang::FunctionDecl* callee = nullptr;
    int paramIndex = -1;
    std::string paramName;  // 【新增】参数名（用于ActualIn/FormalIn显示）

    std::vector<std::pair<ICFGNode*, ICFGEdgeKind>> successors;
    std::vector<std::pair<ICFGNode*, ICFGEdgeKind>> predecessors;

    explicit ICFGNode(ICFGNodeKind k) : kind(k) {}

    std::string GetLabel() const;
    void Dump(const clang::SourceManager* SM = nullptr) const;
};

// ============================================
// 数据依赖信息
// ============================================
struct DataDependency {
    const clang::Stmt* sourceStmt;
    const clang::Stmt* sinkStmt;
    std::string varName;

    enum class DepKind {
        Flow,          // 流依赖 (RAW)
        Anti,          // 反依赖 (WAR)
        Output         // 输出依赖 (WAW)
    } kind;

    DataDependency(const clang::Stmt* src, const clang::Stmt* sink,
                   const std::string& var, DepKind k)
        : sourceStmt(src), sinkStmt(sink), varName(var), kind(k) {}
};

// ============================================
// 控制依赖信息
// ============================================
struct ControlDependency {
    const clang::Stmt* controlStmt;
    const clang::Stmt* dependentStmt;
    bool branchValue;

    ControlDependency(const clang::Stmt* ctrl, const clang::Stmt* dep, bool val)
        : controlStmt(ctrl), dependentStmt(dep), branchValue(val) {}
};

// ============================================
// 程序依赖图节点
// ============================================
class PDGNode {
public:
    const clang::Stmt* stmt;
    const clang::FunctionDecl* func;
    std::vector<DataDependency> dataDeps;
    std::vector<ControlDependency> controlDeps;

    explicit PDGNode(const clang::Stmt* s, const clang::FunctionDecl* f = nullptr)
        : stmt(s), func(f) {}

    void AddDataDep(const DataDependency& dep) { dataDeps.push_back(dep); }
    void AddControlDep(const ControlDependency& dep) { controlDeps.push_back(dep); }
    void Dump(const clang::SourceManager* SM = nullptr) const;
};

// ============================================
// 上下文信息（预留用于上下文敏感分析）
// ============================================
class CallContext {
public:
    std::vector<const clang::CallExpr*> callStack;

    bool operator<(const CallContext& other) const
    {
        return callStack < other.callStack;
    }

    bool operator==(const CallContext& other) const
    {
        return callStack == other.callStack;
    }

    std::string ToString() const;
};

// ============================================
// 路径条件（预留用于路径敏感分析）
// ============================================
class PathCondition {
public:
    std::vector<std::pair<const clang::Stmt*, bool>> conditions;

    void AddCondition(const clang::Stmt* cond, bool value)
    {
        conditions.push_back({cond, value});
    }

    bool IsFeasible() const;
    std::string ToString() const;
};

// ============================================
// Reaching Definitions 分析结果
// ============================================
struct ReachingDefsInfo {
    std::map<const clang::Stmt*,
             std::map<std::string, std::set<const clang::Stmt*>>> reachingDefs;
    std::map<const clang::Stmt*, std::set<std::string>> definitions;
    std::map<const clang::Stmt*, std::set<std::string>> uses;
};

// 数据流追踪辅助方法
struct InterproceduralWorkItem {
    const clang::Stmt* stmt;
    int depth;
    const clang::FunctionDecl* function;
    std::string var;
};

struct ForwardWorkItem {
    const clang::Stmt* defStmt;
    const clang::ParmVarDecl* paramDecl;
    int depth;
    const clang::FunctionDecl* function;
    std::string var;
};
}


#endif // CPPANALYZER_CPGBUILDER_H