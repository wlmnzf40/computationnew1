/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphTester.cpp - 计算图测试器类实现
 */
#include "ComputeGraphTester.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <sstream>
#include <iomanip>

using namespace clang;
using namespace llvm;

namespace compute_graph {

// 全局配置定义
ComputeGraphTestConfig g_cgConfig;

// ============================================
// ComputeGraphTestRunner 实现
// ============================================

ComputeGraphTestRunner::ComputeGraphTestRunner(ASTContext& astCtx,
                                               cpg::CPGContext& cpgCtx)
    : astContext_(astCtx), cpgContext_(cpgCtx)
{}

void ComputeGraphTestRunner::PrintDemoHeader(const std::string& title)
{
    outs() << "\n┌─────────────────────────────────────────────────────────────────┐\n";
    outs() << "│ " << title;
    for (size_t i = title.length(); i < 64; ++i) {
        outs() << " ";
    }
    outs() << "│\n";
    outs() << "└─────────────────────────────────────────────────────────────────┘\n";
}

void ComputeGraphTestRunner::PrintDemoResult(const TestResult& result)
{
    outs() << "\n  Test: " << result.testName << "\n";
    outs() << "  Status: " << (result.passed ? "✓ PASSED" : "✗ FAILED") << "\n";
    if (!result.message.empty()) {
        outs() << "  Message: " << result.message << "\n";
    }
    outs() << "  Stats: ";
    outs() << "Anchors=" << result.anchorCount << ", ";
    outs() << "Graphs=" << result.graphCount << ", ";
    outs() << "Nodes=" << result.nodeCount << ", ";
    outs() << "Edges=" << result.edgeCount << "\n";
}

std::vector<const FunctionDecl*> ComputeGraphTestRunner::CollectFunctions()
{
    std::vector<const FunctionDecl*> funcs;
    SourceManager& sm = astContext_.getSourceManager();

    for (auto* decl : astContext_.getTranslationUnitDecl()->decls()) {
        // 【新增】跳过系统头文件中的声明
        if (decl->getLocation().isValid() &&
            sm.isInSystemHeader(decl->getLocation())) {
            continue;
        }

        // 获取要处理的函数声明
        const FunctionDecl* func = nullptr;

        // 普通函数
        if (auto* funcDecl = dyn_cast<FunctionDecl>(decl)) {
            func = funcDecl;
        }
        // 模板函数 - 获取模板的定义
        else if (auto* funcTemplate = dyn_cast<FunctionTemplateDecl>(decl)) {
            func = funcTemplate->getTemplatedDecl();
        }

        if (func && func->hasBody() && func->isThisDeclarationADefinition()) {
            // 【新增】跳过函数体在系统头文件中的情况
            SourceLocation bodyLoc = func->getBody()->getBeginLoc();
            if (bodyLoc.isValid() && sm.isInSystemHeader(bodyLoc)) {
                continue;
            }

            if (g_cgConfig.targetFunction.empty() ||
                func->getNameAsString() == g_cgConfig.targetFunction) {
                funcs.push_back(func);
            }
        }
    }

    return funcs;
}

void ComputeGraphTestRunner::AnalyzeFunction(const FunctionDecl* func)
{
    outs() << "\n  [Analyzing Function: " << func->getNameAsString() << "]\n";

    // 1. 查找锚点
    AnchorFinder finder(cpgContext_, astContext_);
    auto anchors = finder.FindAnchorsInFunction(func);
    auto rankedAnchors = finder.FilterAndRankAnchors(anchors);

    outs() << "  Found " << anchors.size() << " raw anchors, ";
    outs() << rankedAnchors.size() << " after filtering\n";

    if (g_cgConfig.verbose && !rankedAnchors.empty()) {
        outs() << "\n  Top anchors (by score):\n";
        int count = 0;
        for (const auto& anchor : rankedAnchors) {
            if (count++ >= 10) break;
            outs() << "    [" << count << "] ";
            outs() << "L" << anchor.sourceLine << " ";
            outs() << "score=" << anchor.score << " ";
            outs() << "depth=" << anchor.loopDepth << " ";
            outs() << ComputeNodeKindToString(anchor.expectedKind);
            if (anchor.opCode != OpCode::Unknown) {
                outs() << "(" << OpCodeToString(anchor.opCode) << ")";
            }
            outs() << "\n";
            outs() << "        code: " << anchor.sourceText << "\n";
        }
    }

    // 2. 从锚点构建图
    ComputeGraphBuilder builder(cpgContext_, astContext_);
    builder.SetMaxBackwardDepth(g_cgConfig.maxBackwardDepth);
    builder.SetMaxForwardDepth(g_cgConfig.maxForwardDepth);

    ComputeGraphSet graphSet;

    for (const auto& anchor : rankedAnchors) {
        auto graph = builder.BuildFromAnchor(anchor);
        if (graph && !graph->IsEmpty()) {
            graphSet.AddGraph(graph);

            if (g_cgConfig.verbose) {
                outs() << "    Built graph '" << graph->GetName() << "': ";
                outs() << graph->NodeCount() << " nodes, ";
                outs() << graph->EdgeCount() << " edges\n";
            }
        }
    }

    outs() << "  Built " << graphSet.Size() << " computation graphs\n";

    // 3. 合并重叠的图
    size_t beforeMerge = graphSet.Size();
    MergeOverlappingGraphs(graphSet);
    if (beforeMerge != graphSet.Size()) {
        outs() << "  Merged overlapping graphs: " << beforeMerge << " -> " << graphSet.Size() << "\n";
    }

    // 4. 去重
    size_t beforeDedup = graphSet.Size();
    graphSet.Deduplicate();
    if (beforeDedup != graphSet.Size()) {
        outs() << "  Deduplicated: " << beforeDedup << " -> " << graphSet.Size() << "\n";
    }

    outs() << "  Final: " << graphSet.Size() << " graphs\n";

    // 5. 输出图信息
    if (g_cgConfig.dumpGraphs) {
        for (const auto& graph : graphSet.GetAllGraphs()) {
            outs() << "\n  --- Graph: " << graph->GetName() << " ---\n";
            outs() << "  Nodes: " << graph->NodeCount() << ", Edges: " << graph->EdgeCount() << "\n";

            // 输出图属性
            if (graph->HasProperty("anchor_func")) {
                outs() << "  Function: " << graph->GetProperty("anchor_func") << "\n";
            }
            if (graph->HasProperty("anchor_line")) {
                outs() << "  Anchor Line: " << graph->GetProperty("anchor_line") << "\n";
            }
            if (graph->HasProperty("anchor_code")) {
                outs() << "  Anchor Code: " << graph->GetProperty("anchor_code") << "\n";
            }

            if (g_cgConfig.verbose) {
                graph->Dump();
            }
        }
    }

    // 6. 生成可视化文件
    if (g_cgConfig.visualize) {
        int idx = 0;
        for (const auto& graph : graphSet.GetAllGraphs()) {
            std::string filename = g_cgConfig.outputDir + "/" +
                func->getNameAsString() + "_cg_" + std::to_string(idx++) + ".dot";
            graph->ExportDotFile(filename);
            outs() << "  Generated: " << filename << "\n";
        }
    }
}

void ComputeGraphTestRunner::PrintSummary() const
{
    outs() << "\n";
    outs() << "╔══════════════════════════════════════════════════════════════════╗\n";
    outs() << "║                        Test Summary                              ║\n";
    outs() << "╚══════════════════════════════════════════════════════════════════╝\n\n";

    int passed = 0;
    int failed = 0;
    int totalAnchors = 0;
    int totalGraphs = 0;
    int totalNodes = 0;
    int totalEdges = 0;

    for (const auto& result : results_) {
        if (result.passed) {
            passed++;
        } else {
            failed++;
        }
        totalAnchors += result.anchorCount;
        totalGraphs += result.graphCount;
        totalNodes += result.nodeCount;
        totalEdges += result.edgeCount;
    }

    outs() << "  Tests: " << passed << " passed, " << failed << " failed\n";
    outs() << "  Total anchors found: " << totalAnchors << "\n";
    outs() << "  Total graphs built: " << totalGraphs << "\n";
    outs() << "  Total nodes: " << totalNodes << "\n";
    outs() << "  Total edges: " << totalEdges << "\n\n";

    outs() << "  Detailed Results:\n";
    for (const auto& result : results_) {
        outs() << "    " << (result.passed ? "✓" : "✗") << " ";
        outs() << result.testName << ": " << result.message << "\n";
    }

    outs() << "\n";
}

void ComputeGraphTestRunner::AddResult(const TestResult& result)
{
    results_.push_back(result);
}

// ============================================
// AnchorAnalysisTester 实现
// ============================================

void AnchorAnalysisTester::TestAnchorFinding(cpg::CPGContext& cpgCtx,
                                             ASTContext& astCtx)
{
    outs() << "\n[Testing Anchor Finding]\n";

    AnchorFinder finder(cpgCtx, astCtx);
    auto anchors = finder.FindAllAnchors();

    outs() << "  Total anchors found: " << anchors.size() << "\n";

    // 按类型分组
    std::map<ComputeNodeKind, int> kindCount;
    for (const auto& anchor : anchors) {
        kindCount[anchor.expectedKind]++;
    }

    outs() << "  By type:\n";
    for (const auto& [kind, count] : kindCount) {
        outs() << "    " << ComputeNodeKindToString(kind) << ": " << count << "\n";
    }
}

void AnchorAnalysisTester::TestAnchorRanking(const std::vector<AnchorPoint>& anchors)
{
    outs() << "\n[Testing Anchor Ranking]\n";

    outs() << "  Top 10 anchors by score:\n";
    int count = 0;
    for (const auto& anchor : anchors) {
        if (count++ >= 10) break;
        outs() << "    " << count << ". Score=" << anchor.score;
        outs() << " Depth=" << anchor.loopDepth;
        outs() << " " << anchor.ToString() << "\n";
    }
}

void AnchorAnalysisTester::PrintAnchorDetails(const AnchorPoint& anchor,
                                              ASTContext& astCtx)
{
    outs() << "  Anchor Details:\n";
    outs() << "    Kind: " << ComputeNodeKindToString(anchor.expectedKind) << "\n";
    outs() << "    OpCode: " << OpCodeToString(anchor.opCode) << "\n";
    outs() << "    Loop Depth: " << anchor.loopDepth << "\n";
    outs() << "    In Loop: " << (anchor.isInLoop ? "yes" : "no") << "\n";
    outs() << "    Score: " << anchor.score << "\n";

    if (anchor.stmt) {
        outs() << "    Has statement: yes\n";
    }
}

// ============================================
// GraphBuildingTester 实现
// ============================================

void GraphBuildingTester::TestBuildFromAnchor(ComputeGraphBuilder& builder,
                                              const AnchorPoint& anchor)
{
    outs() << "\n[Testing Build From Anchor]\n";

    auto graph = builder.BuildFromAnchor(anchor);
    if (graph) {
        outs() << "  Graph built successfully\n";
        graph->PrintSummary();
    } else {
        outs() << "  Failed to build graph\n";
    }
}

void GraphBuildingTester::TestBuildFromFunction(ComputeGraphBuilder& builder,
                                                const FunctionDecl* func)
{
    outs() << "\n[Testing Build From Function: " << func->getNameAsString() << "]\n";

    auto graph = builder.BuildFromFunction(func);
    if (graph) {
        outs() << "  Graph built successfully\n";
        graph->PrintSummary();

        auto roots = graph->GetRootNodes();
        auto leaves = graph->GetLeafNodes();

        outs() << "  Root nodes: " << roots.size() << "\n";
        outs() << "  Leaf nodes: " << leaves.size() << "\n";
    }
}

void GraphBuildingTester::TestGraphProperties(const ComputeGraph& graph)
{
    outs() << "\n[Testing Graph Properties]\n";

    outs() << "  Name: " << graph.GetName() << "\n";
    outs() << "  Nodes: " << graph.NodeCount() << "\n";
    outs() << "  Edges: " << graph.EdgeCount() << "\n";
    outs() << "  Is Empty: " << (graph.IsEmpty() ? "yes" : "no") << "\n";

    auto sig = graph.ComputeCanonicalSignature();
    outs() << "  Signature length: " << sig.length() << "\n";
}

// ============================================
// GraphOperationsTester 实现
// ============================================

void GraphOperationsTester::TestMerge(const ComputeGraph& g1, const ComputeGraph& g2)
{
    outs() << "\n[Testing Graph Merge]\n";

    outs() << "  Graph 1: " << g1.NodeCount() << " nodes, " << g1.EdgeCount() << " edges\n";
    outs() << "  Graph 2: " << g2.NodeCount() << " nodes, " << g2.EdgeCount() << " edges\n";

    auto merged = ComputeGraphMerger::Merge(g1, g2);
    outs() << "  Merged: " << merged->NodeCount() << " nodes, ";
    outs() << merged->EdgeCount() << " edges\n";
}

void GraphOperationsTester::TestDeduplicate(ComputeGraphSet& graphSet)
{
    outs() << "\n[Testing Deduplication]\n";

    size_t before = graphSet.Size();
    graphSet.Deduplicate();
    size_t after = graphSet.Size();

    outs() << "  Before: " << before << " graphs\n";
    outs() << "  After: " << after << " graphs\n";
    outs() << "  Removed: " << (before - after) << " duplicates\n";
}

void GraphOperationsTester::TestSubgraphExtraction(const ComputeGraph& graph)
{
    outs() << "\n[Testing Subgraph Extraction]\n";

    std::set<ComputeNode::NodeId> opNodeIds;
    for (const auto& node : graph.GetAllNodes()) {
        if (node->IsOperationNode()) {
            opNodeIds.insert(node->id);
        }
    }

    if (!opNodeIds.empty()) {
        auto subgraph = graph.ExtractSubgraph(opNodeIds);
        outs() << "  Original: " << graph.NodeCount() << " nodes\n";
        outs() << "  Operation subgraph: " << subgraph.NodeCount() << " nodes\n";
    }
}

void GraphOperationsTester::TestTopologicalSort(const ComputeGraph& graph)
{
    outs() << "\n[Testing Topological Sort]\n";

    auto sorted = graph.TopologicalSort();
    outs() << "  Sorted order (" << sorted.size() << " nodes):\n    ";

    for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
        outs() << sorted[i]->GetKindName() << " ";
    }
    if (sorted.size() > 10) {
        outs() << "...";
    }
    outs() << "\n";
}

// // ============================================
// // PatternMatchingTester 实现
// // ============================================
//
// void PatternMatchingTester::RegisterTestPatterns(PatternMatcher& matcher)
// {
//     // 标量加法模式
//     RewritePattern scalarAdd;
//     scalarAdd.name = "scalar_add";
//     scalarAdd.pattern = {
//         {ComputeNodeKind::BinaryOp, OpCode::Add, "", 0, {}}
//     };
//     matcher.RegisterPattern(scalarAdd);
//
//     // 标量乘法模式
//     RewritePattern scalarMul;
//     scalarMul.name = "scalar_mul";
//     scalarMul.pattern = {
//         {ComputeNodeKind::BinaryOp, OpCode::Mul, "", 0, {}}
//     };
//     matcher.RegisterPattern(scalarMul);
//
//     // 数组访问模式
//     RewritePattern arrayAccess;
//     arrayAccess.name = "array_access";
//     arrayAccess.pattern = {
//         {ComputeNodeKind::ArrayAccess, OpCode::Unknown, "", 0, {}}
//     };
//     matcher.RegisterPattern(arrayAccess);
//
//     // BF16点积模式
//     RewritePattern bf16DotProduct;
//     bf16DotProduct.name = "bf16_dot_product";
//     bf16DotProduct.pattern = {
//         {ComputeNodeKind::ArrayAccess, OpCode::Unknown, "", 0, {}},
//         {ComputeNodeKind::ArrayAccess, OpCode::Unknown, "", 1, {}},
//         {ComputeNodeKind::Call, OpCode::Unknown, "bf16_to_fp32", 2, {0}},
//         {ComputeNodeKind::Call, OpCode::Unknown, "bf16_to_fp32", 3, {1}},
//         {ComputeNodeKind::BinaryOp, OpCode::Mul, "", 4, {2, 3}},
//         {ComputeNodeKind::BinaryOp, OpCode::Add, "reduction", 5, {4}}
//     };
//     matcher.RegisterPattern(bf16DotProduct);
// }
//
// void PatternMatchingTester::TestScalarAddPattern(PatternMatcher& matcher,
//                                                   const ComputeGraph& graph)
// {
//     auto matches = matcher.FindMatches(graph, "scalar_add");
//     outs() << "  scalar_add matches: " << matches.size() << "\n";
// }
//
// void PatternMatchingTester::TestArrayAccessPattern(PatternMatcher& matcher,
//                                                     const ComputeGraph& graph)
// {
//     auto matches = matcher.FindMatches(graph, "array_access");
//     outs() << "  array_access matches: " << matches.size() << "\n";
// }
//
// void PatternMatchingTester::TestLoopPattern(PatternMatcher& matcher,
//                                             const ComputeGraph& graph)
// {
//     int loopNodes = 0;
//     for (const auto& node : graph.GetAllNodes()) {
//         if (node->loopDepth > 0) {
//             loopNodes++;
//         }
//     }
//     outs() << "  Nodes in loops: " << loopNodes << "\n";
// }
//
// void PatternMatchingTester::TestBF16DotProductPattern(PatternMatcher& matcher,
//                                                        const ComputeGraph& graph)
// {
//     auto matches = matcher.FindMatches(graph, "bf16_dot_product");
//     outs() << "  bf16_dot_product matches: " << matches.size() << "\n";
//
//     if (!matches.empty()) {
//         outs() << "  Matched nodes:\n";
//         for (const auto& match : matches) {
//             for (const auto& [captureId, nodeId] : match) {
//                 auto node = graph.GetNode(nodeId);
//                 if (node) {
//                     outs() << "    [" << captureId << "] -> "
//                            << node->GetKindName() << ": " << node->name << "\n";
//                 }
//             }
//         }
//     }
// }

// ============================================
// VisualizationGenerator 实现
// ============================================

void VisualizationGenerator::GenerateAllGraphDots(const ComputeGraphSet& graphSet,
                                                   const std::string& outputDir)
{
    std::error_code ec = sys::fs::create_directories(outputDir);
    if (ec) {
        errs() << "Failed to create output directory: " << outputDir << "\n";
        return;
    }

    int idx = 0;
    for (const auto& graph : graphSet.GetAllGraphs()) {
        std::string filename = outputDir + "/compute_graph_" +
            std::to_string(idx++) + ".dot";
        graph->ExportDotFile(filename);
    }

    outs() << "Generated " << idx << " DOT files in " << outputDir << "\n";
}

void VisualizationGenerator::GenerateCombinedDot(
    const std::vector<std::shared_ptr<ComputeGraph>>& graphs,
    const std::string& filename)
{
    std::error_code EC;
    raw_fd_ostream out(filename, EC);
    if (EC) {
        errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "digraph CombinedGraphs {\n";
    out << "  rankdir=TB;\n";
    out << "  compound=true;\n";
    out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n\n";

    int graphIdx = 0;
    for (const auto& graph : graphs) {
        out << "  subgraph cluster_" << graphIdx << " {\n";
        out << "    label=\"" << graph->GetName() << "\";\n";
        out << "    style=filled;\n";
        out << "    color=lightgrey;\n";

        for (const auto& node : graph->GetAllNodes()) {
            out << "    n" << graphIdx << "_" << node->id;
            out << " [label=\"" << node->GetLabel() << "\"];\n";
        }

        for (const auto& edge : graph->GetAllEdges()) {
            out << "    n" << graphIdx << "_" << edge->sourceId;
            out << " -> n" << graphIdx << "_" << edge->targetId;
            out << " [label=\"" << edge->GetLabel() << "\"];\n";
        }

        out << "  }\n\n";
        graphIdx++;
    }

    out << "}\n";

    outs() << "Generated combined DOT file: " << filename << "\n";
}

void VisualizationGenerator::GenerateHTMLReport(const std::vector<TestResult>& results,
                                                 const std::string& filename)
{
    std::error_code EC;
    raw_fd_ostream out(filename, EC);
    if (EC) {
        errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "<!DOCTYPE html>\n<html>\n<head>\n";
    out << "<title>Compute Graph Analysis Report</title>\n";
    out << "<style>\n";
    out << "body { font-family: Arial, sans-serif; margin: 20px; background: #0d1117; color: #e6edf3; }\n";
    out << "table { border-collapse: collapse; width: 100%; }\n";
    out << "th, td { border: 1px solid #30363d; padding: 8px; text-align: left; }\n";
    out << "th { background-color: #21262d; color: #58a6ff; }\n";
    out << ".passed { color: #3fb950; }\n";
    out << ".failed { color: #f85149; }\n";
    out << "h1 { color: #58a6ff; }\n";
    out << "</style>\n</head>\n<body>\n";

    out << "<h1>Compute Graph Analysis Report</h1>\n";
    out << "<table>\n";
    out << "<tr><th>Test</th><th>Status</th><th>Anchors</th>";
    out << "<th>Graphs</th><th>Nodes</th><th>Edges</th><th>Message</th></tr>\n";

    for (const auto& result : results) {
        out << "<tr>";
        out << "<td>" << result.testName << "</td>";
        out << "<td class=\"" << (result.passed ? "passed" : "failed") << "\">";
        out << (result.passed ? "PASSED" : "FAILED") << "</td>";
        out << "<td>" << result.anchorCount << "</td>";
        out << "<td>" << result.graphCount << "</td>";
        out << "<td>" << result.nodeCount << "</td>";
        out << "<td>" << result.edgeCount << "</td>";
        out << "<td>" << result.message << "</td>";
        out << "</tr>\n";
    }

    out << "</table>\n</body>\n</html>\n";
}

} // namespace compute_graph