/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef CPG_ANALYSIS_TESTERS_H
#define CPG_ANALYSIS_TESTERS_H

#include "code_property_graph/CPGAnnotation.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <string>
#include <vector>

// ============================================
// 测试配置结构体
// ============================================
struct CPGTestConfig {
    bool dumpICFG = false;
    bool dumpPDG = false;
    bool dumpCPG = true;
    bool visualize = false;
    bool testDataFlow = true;
    bool testControlFlow = true;
    bool testInterprocedural = true;
    bool verbose = false;
    std::string outputDir = ".";
    std::string targetFunction = "";
};

// 全局配置（在CPGAnalysisTool.cpp中定义）
extern CPGTestConfig g_config;

// ============================================
// 辅助类：打印分隔线
// ============================================
class SectionPrinter {
public:
    static void PrintHeader(const std::string& title);
    static void PrintSubHeader(const std::string& title);
    static void PrintSeparator();
};

// ============================================
// 辅助函数
// ============================================
void PrintEdgeKind(cpg::ICFGEdgeKind kind);

// ============================================
// ICFG功能测试
// ============================================
class ICFGTester {
public:
    static void TestFeatures(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);

private:
    static void TestEntryExitNodes(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
    static void TestSuccessorsPredecessors(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
    static void TestCFGRetrieval(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
};

// ============================================
// PDG功能测试
// ============================================
class PDGTester {
public:
    static void TestFeatures(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);

private:
    static void CountDependencies(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx,
                                   int& pdgNodes, int& dataDeps, int& controlDeps);
};

// ============================================
// 数据流分析测试
// ============================================
class DataFlowTester {
public:
    static void TestAnalysis(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx,
                              clang::ASTContext& astContext);

private:
    static void CollectDefsAndUses(
        clang::FunctionDecl* func,
        std::vector<std::pair<const clang::Stmt*, std::string>>& defs,
        std::vector<std::pair<const clang::Expr*, std::string>>& uses);

    static void TestDefinitionsAndUses(
        const std::vector<std::pair<const clang::Stmt*, std::string>>& defs,
        const std::vector<std::pair<const clang::Expr*, std::string>>& uses,
        cpg::CPGContext& cpgCtx);

    static void TestDataFlowPath(
        const std::vector<std::pair<const clang::Stmt*, std::string>>& defs,
        cpg::CPGContext& cpgCtx);

    static void TestExtractVariables(
        const std::vector<std::pair<const clang::Expr*, std::string>>& uses,
        cpg::CPGContext& cpgCtx);
};

// ============================================
// 控制流分析测试
// ============================================
class ControlFlowTester {
public:
    static void TestAnalysis(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);

private:
    static void TestPathExistence(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
    static void TestFindAllPaths(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
    static void TestControlStatements(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
};

// ============================================
// 跨函数分析测试
// ============================================
class InterproceduralTester {
public:
    static void TestAnalysis(const std::vector<clang::FunctionDecl*>& funcs,
                              cpg::CPGContext& cpgCtx);

private:
    static void TestCallGraphTraversal(const std::vector<clang::FunctionDecl*>& funcs,
                                        cpg::CPGContext& cpgCtx);
    static void TestInterproceduralDataFlow(const std::vector<clang::FunctionDecl*>& funcs,
                                             cpg::CPGContext& cpgCtx);
    static void TestForwardSlicing(const std::vector<clang::FunctionDecl*>& funcs,
                                    cpg::CPGContext& cpgCtx);

    static void TestCallArgumentTrace(const clang::CallExpr* call, cpg::CPGContext& cpgCtx);
    static void TestCalleeParameterUsages(const clang::CallExpr* call, cpg::CPGContext& cpgCtx);
    static void TestSingleCall(const clang::CallExpr* call, cpg::CPGContext& cpgCtx);
    static std::vector<const clang::CallExpr*> CollectCallExprs(clang::FunctionDecl* func);
    static void TestFunctionInterproceduralFlow(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);

    struct FirstDefinitionInfo {
        const clang::Stmt* stmt = nullptr;
        std::string varName;
    };
    static FirstDefinitionInfo ExtractDefinitionFromAssignment(clang::BinaryOperator* binOp);
    static FirstDefinitionInfo FindFirstDefinition(clang::FunctionDecl* func);
    static void TestFunctionForwardSlice(clang::FunctionDecl* func, cpg::CPGContext& cpgCtx);
};

// ============================================
// CPGBuilder测试
// ============================================
class CPGBuilderTester {
public:
    static void TestBuildForTranslationUnit(clang::ASTContext& context);
    static void TestBuildForFunction(clang::ASTContext& context);
};

// ============================================
// 工具类测试
// ============================================
class UtilityClassTester {
public:
    static void TestCallContext();
    static void TestPathCondition();
};

#endif // CPG_ANALYSIS_TESTERS_H