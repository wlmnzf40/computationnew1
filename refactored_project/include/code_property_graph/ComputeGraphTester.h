/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphTester.h - 计算图分析测试器头文件
 */
#ifndef COMPUTE_GRAPH_TESTER_H
#define COMPUTE_GRAPH_TESTER_H

#include "ComputeGraph.h"
#include "code_property_graph/CPGAnnotation.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <string>
#include <vector>

namespace compute_graph {

// ============================================
// 测试配置
// ============================================
struct ComputeGraphTestConfig {
    bool verbose = true;
    bool dumpGraphs = true;
    bool visualize = true;
    bool testPatternMatching = true;
    std::string outputDir = ".";
    std::string targetFunction = "";
    int maxBackwardDepth = 5;
    int maxForwardDepth = 5;
};

// 全局配置
extern ComputeGraphTestConfig g_cgConfig;

// ============================================
// 测试结果
// ============================================
struct TestResult {
    std::string testName;
    bool passed = true;
    std::string message;
    int anchorCount = 0;
    int graphCount = 0;
    int nodeCount = 0;
    int edgeCount = 0;
};

// ============================================
// Demo案例信息
// ============================================
struct DemoCase {
    std::string name;
    std::string description;
    std::string sourceFile;
    std::string codePattern;
    std::vector<std::string> expectedNodeTypes;
    std::vector<std::string> expectedEdges;
};

// ============================================
// 计算图测试运行器
// ============================================
class ComputeGraphTestRunner {
public:
    ComputeGraphTestRunner(clang::ASTContext& astCtx, cpg::CPGContext& cpgCtx);

    // 获取测试结果
    const std::vector<TestResult>& GetResults() const { return results_; }

    // 打印测试摘要
    void PrintSummary() const;

    // 辅助方法
    void PrintDemoHeader(const std::string& title);
    void PrintDemoResult(const TestResult& result);
    void AnalyzeFunction(const clang::FunctionDecl* func);
    std::vector<const clang::FunctionDecl*> CollectFunctions();
    void AddResult(const TestResult& result);

protected:
    clang::ASTContext& astContext_;
    cpg::CPGContext& cpgContext_;
    std::vector<TestResult> results_;
};

// ============================================
// 锚点分析测试器
// ============================================
class AnchorAnalysisTester {
public:
    static void TestAnchorFinding(cpg::CPGContext& cpgCtx,
                                  clang::ASTContext& astCtx);
    static void TestAnchorRanking(const std::vector<AnchorPoint>& anchors);
    static void PrintAnchorDetails(const AnchorPoint& anchor,
                                   clang::ASTContext& astCtx);
};

// ============================================
// 图构建测试器
// ============================================
class GraphBuildingTester {
public:
    static void TestBuildFromAnchor(ComputeGraphBuilder& builder,
                                    const AnchorPoint& anchor);
    static void TestBuildFromFunction(ComputeGraphBuilder& builder,
                                      const clang::FunctionDecl* func);
    static void TestGraphProperties(const ComputeGraph& graph);
};

// ============================================
// 图操作测试器
// ============================================
class GraphOperationsTester {
public:
    static void TestMerge(const ComputeGraph& g1, const ComputeGraph& g2);
    static void TestDeduplicate(ComputeGraphSet& graphSet);
    static void TestSubgraphExtraction(const ComputeGraph& graph);
    static void TestTopologicalSort(const ComputeGraph& graph);
};

// // ============================================
// // 模式匹配测试器
// // ============================================
// class PatternMatchingTester {
// public:
//     static void RegisterTestPatterns(PatternMatcher& matcher);
//     static void TestScalarAddPattern(PatternMatcher& matcher,
//                                      const ComputeGraph& graph);
//     static void TestArrayAccessPattern(PatternMatcher& matcher,
//                                        const ComputeGraph& graph);
//     static void TestLoopPattern(PatternMatcher& matcher,
//                                 const ComputeGraph& graph);
//     static void TestBF16DotProductPattern(PatternMatcher& matcher,
//                                           const ComputeGraph& graph);
// };

// ============================================
// 可视化生成器
// ============================================
class VisualizationGenerator {
public:
    static void GenerateAllGraphDots(const ComputeGraphSet& graphSet,
                                     const std::string& outputDir);
    static void GenerateCombinedDot(const std::vector<std::shared_ptr<ComputeGraph>>& graphs,
                                    const std::string& filename);
    static void GenerateHTMLReport(const std::vector<TestResult>& results,
                                   const std::string& filename);
};

} // namespace compute_graph

#endif // COMPUTE_GRAPH_TESTER_H