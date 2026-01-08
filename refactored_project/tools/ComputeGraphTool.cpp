/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraphTool.cpp - 计算图分析工具主程序
 *
 * 本文件包含:
 * 1. Clang工具框架 (ASTConsumer, FrontendAction)
 * 2. 命令行参数解析
 * 3. 所有Demo的运行逻辑
 *
 * 使用方法:
 *   ./ComputeGraphTool <input.cpp> [options]
 *
 * 输入文件示例:
 *   - simple_arithmetic_test.cpp
 *   - loop_array_test.cpp
 *   - nested_loop_test.cpp
 *   - conditional_branch_test.cpp
 *   - interprocedural_test.cpp
 *   - bf16_dot_product_test.cpp
 */

#include "code_property_graph/ComputeGraphTester.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace compute_graph;

// ============================================
// 命令行选项
// ============================================
static cl::OptionCategory ToolCategory("Compute Graph Analysis Tool Options");

static cl::opt<bool> OptVerbose("verbose",
    cl::desc("Enable verbose output"),
    cl::init(true), cl::cat(ToolCategory));

static cl::opt<bool> OptDumpGraphs("dump-graphs",
    cl::desc("Dump computation graphs"),
    cl::init(true), cl::cat(ToolCategory));

static cl::opt<bool> OptVisualize("visualize",
    cl::desc("Generate DOT files for visualization"),
    cl::init(true), cl::cat(ToolCategory));

static cl::opt<bool> OptTestPatterns("test-patterns",
    cl::desc("Run pattern matching tests"),
    cl::init(true), cl::cat(ToolCategory));

static cl::opt<std::string> OptOutputDir("output-dir",
    cl::desc("Output directory for visualization files"),
    cl::init("."), cl::cat(ToolCategory));

static cl::opt<std::string> OptTargetFunction("function",
    cl::desc("Analyze only the specified function"),
    cl::init(""), cl::cat(ToolCategory));

static cl::opt<int> OptMaxDepth("max-depth",
    cl::desc("Maximum traversal depth for graph building"),
    cl::init(5), cl::cat(ToolCategory));

static cl::opt<bool> OptRunBF16Demo("bf16-demo",
    cl::desc("Run BF16 dot product demo with manual graph construction"),
    cl::init(false), cl::cat(ToolCategory));

// ============================================
// 初始化全局配置
// ============================================
static void InitializeConfig()
{
    g_cgConfig.verbose = OptVerbose;
    g_cgConfig.dumpGraphs = OptDumpGraphs;
    g_cgConfig.visualize = OptVisualize;
    g_cgConfig.testPatternMatching = OptTestPatterns;
    g_cgConfig.outputDir = OptOutputDir;
    g_cgConfig.targetFunction = OptTargetFunction;
    g_cgConfig.maxBackwardDepth = OptMaxDepth;
    g_cgConfig.maxForwardDepth = OptMaxDepth;
}

// ============================================
// 打印工具信息
// ============================================
static void PrintToolBanner()
{
    outs() << "\n";
    outs() << "╔══════════════════════════════════════════════════════════════════╗\n";
    outs() << "║             Compute Graph Analysis Tool v1.0                     ║\n";
    outs() << "╚══════════════════════════════════════════════════════════════════╝\n\n";

    outs() << "Configuration:\n";
    outs() << "  Verbose: " << (g_cgConfig.verbose ? "yes" : "no") << "\n";
    outs() << "  Dump Graphs: " << (g_cgConfig.dumpGraphs ? "yes" : "no") << "\n";
    outs() << "  Visualize: " << (g_cgConfig.visualize ? "yes" : "no") << "\n";
    outs() << "  Pattern Matching: " << (g_cgConfig.testPatternMatching ? "yes" : "no") << "\n";
    outs() << "  Output Dir: " << g_cgConfig.outputDir << "\n";
    outs() << "  Max Depth: " << g_cgConfig.maxBackwardDepth << "\n";
    if (!g_cgConfig.targetFunction.empty()) {
        outs() << "  Target Function: " << g_cgConfig.targetFunction << "\n";
    }
    outs() << "\n";
}

// ============================================
// Demo运行器类
// ============================================
class DemoRunner {
public:
    DemoRunner(ASTContext& astCtx, cpg::CPGContext& cpgCtx)
        : astContext(astCtx), cpgContext(cpgCtx) {}

    // 运行所有Demo
    void RunAllDemos()
    {
        PrintHeader("Starting Compute Graph Analysis");

        // 收集函数
        CollectFunctions();
        outs() << "Found " << functions.size() << " functions to analyze\n\n";

        // 构建全局ICFG
        RunDemoBuildGlobalICFG();

        // 分析每个函数
        RunDemoAnalyzeFunctions();

        // // 模式匹配测试
        // if (g_cgConfig.testPatternMatching) {
        //     RunDemoPatternMatching();
        // }
        //
        // // BF16手动构建Demo
        // if (OptRunBF16Demo) {
        //     RunDemoBF16DotProduct();
        // }

        // 打印统计
        RunDemoPrintStatistics();

        PrintHeader("Analysis Complete");
    }

private:
    ASTContext& astContext;
    cpg::CPGContext& cpgContext;
    std::vector<FunctionDecl*> functions;
    std::vector<TestResult> results;

    void PrintHeader(const std::string& title)
    {
        outs() << "\n";
        outs() << "╔══════════════════════════════════════════════════════════════════╗\n";
        outs() << "║ " << title;
        for (size_t i = title.length(); i < 67; ++i) outs() << " ";
        outs() << "║\n";
        outs() << "╚══════════════════════════════════════════════════════════════════╝\n";
    }

    void PrintSubHeader(const std::string& title)
    {
        outs() << "\n┌─────────────────────────────────────────────────────────────────┐\n";
        outs() << "│ " << title;
        for (size_t i = title.length(); i < 64; ++i) outs() << " ";
        outs() << "│\n";
        outs() << "└─────────────────────────────────────────────────────────────────┘\n";
    }

    // 收集需要分析的函数
    void CollectFunctions()
    {
        SourceManager& sm = astContext.getSourceManager();

        for (auto* decl : astContext.getTranslationUnitDecl()->decls()) {
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

            if (!func || !func->hasBody() || !func->isThisDeclarationADefinition()) {
                continue;
            }

            // 【新增】跳过函数体在系统头文件中的情况
            SourceLocation bodyLoc = func->getBody()->getBeginLoc();
            if (bodyLoc.isValid() && sm.isInSystemHeader(bodyLoc)) {
                continue;
            }

            if (!g_cgConfig.targetFunction.empty() &&
                func->getNameAsString() != g_cgConfig.targetFunction) {
                continue;
            }
            functions.push_back(const_cast<FunctionDecl*>(func));
        }
    }

    // ========================================
    // Demo 1: 构建全局ICFG
    // ========================================
    void RunDemoBuildGlobalICFG()
    {
        PrintSubHeader("Demo 1: Building Global ICFG");
        cpgContext.BuildICFGForTranslationUnit();
        outs() << "Global ICFG constructed successfully\n";
    }

    // ========================================
    // Demo 2: 分析每个函数
    // ========================================
    void RunDemoAnalyzeFunctions()
    {
        for (auto* func : functions) {
            std::string funcName = func->getNameAsString();
            PrintSubHeader("Demo 2: Analyzing Function: " + funcName);

            // 构建CPG
            cpgContext.BuildCPG(func);

            // 查找锚点
            AnchorFinder finder(cpgContext, astContext);
            auto anchors = finder.FindAnchorsInFunction(func);
            auto rankedAnchors = finder.FilterAndRankAnchors(anchors);

            outs() << "  Found " << anchors.size() << " raw anchors, ";
            outs() << rankedAnchors.size() << " after filtering\n";

            // 构建计算图
            ComputeGraphBuilder builder(cpgContext, astContext);
            builder.SetMaxBackwardDepth(g_cgConfig.maxBackwardDepth);
            builder.SetMaxForwardDepth(g_cgConfig.maxForwardDepth);

            ComputeGraphSet graphSet;
            TestResult result;
            result.testName = funcName;
            result.passed = true;

            for (const auto& anchor : rankedAnchors) {
                auto graph = builder.BuildFromAnchor(anchor);
                if (graph && !graph->IsEmpty()) {
                    graphSet.AddGraph(graph);
                    result.anchorCount++;
                }
            }

            // 去重并合并重叠的图
            size_t beforeDedup = graphSet.Size();
            graphSet.Deduplicate();
            graphSet.MergeOverlapping();  // 合并有共享节点的图

            outs() << "  Built " << beforeDedup << " graphs, ";
            outs() << graphSet.Size() << " after dedup & merge\n";

            result.graphCount = graphSet.Size();

            // 统计节点和边
            for (const auto& graph : graphSet.GetAllGraphs()) {
                result.nodeCount += graph->NodeCount();
                result.edgeCount += graph->EdgeCount();

                if (g_cgConfig.dumpGraphs) {
                    graph->PrintSummary();
                    if (g_cgConfig.verbose) {
                        graph->Dump();
                    }
                }
            }

            // 生成可视化
            if (g_cgConfig.visualize) {
                std::error_code ec = sys::fs::create_directories(g_cgConfig.outputDir);
                if (!ec) {
                    int idx = 0;
                    for (const auto& graph : graphSet.GetAllGraphs()) {
                        std::string filename = g_cgConfig.outputDir + "/" +
                            funcName + "_cg_" + std::to_string(idx++) + ".dot";
                        graph->ExportDotFile(filename);
                        outs() << "  Generated: " << filename << "\n";
                    }
                }
            }

            result.message = "Analyzed " + std::to_string(result.graphCount) + " graphs";
            results.push_back(result);
        }
    }

    // // ========================================
    // // Demo 3: 模式匹配测试
    // // ========================================
    // void RunDemoPatternMatching()
    // {
    //     PrintSubHeader("Demo 3: Pattern Matching");
    //
    //     PatternMatcher matcher;
    //     PatternMatchingTester::RegisterTestPatterns(matcher);
    //
    //     outs() << "Registered patterns: ";
    //     for (const auto& name : matcher.GetRegisteredPatterns()) {
    //         outs() << name << " ";
    //     }
    //     outs() << "\n\n";
    //
    //     // 在所有函数中查找模式
    //     int totalMatches = 0;
    //
    //     for (auto* func : functions) {
    //         AnchorFinder finder(cpgContext, astContext);
    //         auto anchors = finder.FindAnchorsInFunction(func);
    //
    //         ComputeGraphBuilder builder(cpgContext, astContext);
    //
    //         for (const auto& anchor : anchors) {
    //             auto graph = builder.BuildFromAnchor(anchor);
    //             if (graph && !graph->IsEmpty()) {
    //                 // 测试各种模式
    //                 auto addMatches = matcher.FindMatches(*graph, "scalar_add");
    //                 auto mulMatches = matcher.FindMatches(*graph, "scalar_mul");
    //                 auto arrMatches = matcher.FindMatches(*graph, "array_access");
    //
    //                 int funcMatches = addMatches.size() + mulMatches.size() + arrMatches.size();
    //                 if (funcMatches > 0) {
    //                     outs() << "  " << func->getNameAsString() << ": ";
    //                     outs() << addMatches.size() << " adds, ";
    //                     outs() << mulMatches.size() << " muls, ";
    //                     outs() << arrMatches.size() << " array accesses\n";
    //                     totalMatches += funcMatches;
    //                 }
    //             }
    //         }
    //     }
    //
    //     outs() << "\nTotal pattern matches: " << totalMatches << "\n";
    // }
    //
    // // ========================================
    // // Demo 4: BF16点积手动构建
    // // ========================================
    // void RunDemoBF16DotProduct()
    // {
    //     PrintSubHeader("Demo 4: BF16 Dot Product - Manual Graph Construction");
    //
    //     /*
    //      * 原始代码:
    //      * for (i = 0; i < n; ++i) {
    //      *     sumf += (ggml_float)(GGML_BF16_TO_FP32(x[i]) *
    //      *                          GGML_BF16_TO_FP32(y[i]));
    //      * }
    //      */
    //
    //     outs() << "\n";
    //     outs() << "  原始代码: ggml_vec_dot_bf16\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //     outs() << "  for (i = 0; i < n; ++i) {\n";
    //     outs() << "      sumf += (ggml_float)(GGML_BF16_TO_FP32(x[i]) *\n";
    //     outs() << "                           GGML_BF16_TO_FP32(y[i]));\n";
    //     outs() << "  }\n\n";
    //
    //     // 创建计算图
    //     auto graph = std::make_shared<ComputeGraph>("bf16_dot_product_loop_body");
    //
    //     outs() << "  Step 1: 创建节点 (ComputeNode)\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //
    //     // 节点1: 循环归纳变量 i
    //     auto nodeI = graph->CreateNode(ComputeNodeKind::LoopInduction);
    //     nodeI->name = "i";
    //     nodeI->loopDepth = 1;
    //     nodeI->SetProperty("init", "0");
    //     nodeI->SetProperty("step", "1");
    //     nodeI->SetProperty("bound", "n");
    //     outs() << "  [" << nodeI->id << "] LoopInduction: i (loopDepth=1)\n";
    //
    //     // 节点2: 数组访问 x[i]
    //     auto nodeXi = graph->CreateNode(ComputeNodeKind::ArrayAccess);
    //     nodeXi->name = "x[i]";
    //     nodeXi->loopDepth = 1;
    //     nodeXi->dataType.baseType = DataTypeInfo::BaseType::Int16;
    //     nodeXi->dataType.bitWidth = 16;
    //     nodeXi->SetProperty("array_name", "x");
    //     nodeXi->SetProperty("element_type", "ggml_bf16_t");
    //     outs() << "  [" << nodeXi->id << "] ArrayAccess: x[i] (bf16)\n";
    //
    //     // 节点3: 数组访问 y[i]
    //     auto nodeYi = graph->CreateNode(ComputeNodeKind::ArrayAccess);
    //     nodeYi->name = "y[i]";
    //     nodeYi->loopDepth = 1;
    //     nodeYi->dataType.baseType = DataTypeInfo::BaseType::Int16;
    //     nodeYi->dataType.bitWidth = 16;
    //     nodeYi->SetProperty("array_name", "y");
    //     outs() << "  [" << nodeYi->id << "] ArrayAccess: y[i] (bf16)\n";
    //
    //     // 节点4: 类型转换 bf16_to_fp32(x[i])
    //     auto nodeConvX = graph->CreateNode(ComputeNodeKind::Call);
    //     nodeConvX->name = "bf16_to_fp32_x";
    //     nodeConvX->loopDepth = 1;
    //     nodeConvX->dataType.baseType = DataTypeInfo::BaseType::Float;
    //     nodeConvX->dataType.bitWidth = 32;
    //     nodeConvX->SetProperty("callee", "GGML_BF16_TO_FP32");
    //     nodeConvX->SetProperty("is_type_conversion", "true");
    //     outs() << "  [" << nodeConvX->id << "] Call: GGML_BF16_TO_FP32(x[i]) -> fp32\n";
    //
    //     // 节点5: 类型转换 bf16_to_fp32(y[i])
    //     auto nodeConvY = graph->CreateNode(ComputeNodeKind::Call);
    //     nodeConvY->name = "bf16_to_fp32_y";
    //     nodeConvY->loopDepth = 1;
    //     nodeConvY->dataType.baseType = DataTypeInfo::BaseType::Float;
    //     nodeConvY->dataType.bitWidth = 32;
    //     nodeConvY->SetProperty("callee", "GGML_BF16_TO_FP32");
    //     outs() << "  [" << nodeConvY->id << "] Call: GGML_BF16_TO_FP32(y[i]) -> fp32\n";
    //
    //     // 节点6: 乘法
    //     auto nodeMul = graph->CreateNode(ComputeNodeKind::BinaryOp);
    //     nodeMul->name = "multiply";
    //     nodeMul->opCode = OpCode::Mul;
    //     nodeMul->loopDepth = 1;
    //     nodeMul->dataType.baseType = DataTypeInfo::BaseType::Float;
    //     nodeMul->dataType.bitWidth = 32;
    //     outs() << "  [" << nodeMul->id << "] BinaryOp: Mul (fp32 * fp32)\n";
    //
    //     // 节点7: 累加变量 sumf (输入)
    //     auto nodeSumfIn = graph->CreateNode(ComputeNodeKind::Variable);
    //     nodeSumfIn->name = "sumf_in";
    //     nodeSumfIn->loopDepth = 1;
    //     nodeSumfIn->dataType.baseType = DataTypeInfo::BaseType::Double;
    //     nodeSumfIn->dataType.bitWidth = 64;
    //     nodeSumfIn->SetProperty("is_accumulator", "true");
    //     nodeSumfIn->SetProperty("loop_carried", "true");
    //     outs() << "  [" << nodeSumfIn->id << "] Variable: sumf_in (accumulator, loop-carried)\n";
    //
    //     // 节点8: 加法 (累加)
    //     auto nodeAdd = graph->CreateNode(ComputeNodeKind::BinaryOp);
    //     nodeAdd->name = "accumulate";
    //     nodeAdd->opCode = OpCode::Add;
    //     nodeAdd->loopDepth = 1;
    //     nodeAdd->dataType.baseType = DataTypeInfo::BaseType::Double;
    //     nodeAdd->SetProperty("is_reduction", "true");
    //     nodeAdd->SetProperty("reduction_op", "sum");
    //     outs() << "  [" << nodeAdd->id << "] BinaryOp: Add (reduction)\n";
    //
    //     // 节点9: 存储
    //     auto nodeStore = graph->CreateNode(ComputeNodeKind::Store);
    //     nodeStore->name = "store_sumf";
    //     nodeStore->loopDepth = 1;
    //     outs() << "  [" << nodeStore->id << "] Store: sumf\n";
    //
    //     outs() << "\n  Step 2: 创建边 (ComputeEdge)\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //
    //     // 边1-2: i -> x[i], i -> y[i]
    //     graph->AddEdge(nodeI->id, nodeXi->id, ComputeEdgeKind::DataFlow, "i");
    //     graph->AddEdge(nodeI->id, nodeYi->id, ComputeEdgeKind::DataFlow, "i");
    //     outs() << "  DataFlow: i -> x[i], i -> y[i]\n";
    //
    //     // 边3-4: x[i] -> conv, y[i] -> conv
    //     graph->AddEdge(nodeXi->id, nodeConvX->id, ComputeEdgeKind::DataFlow, "x[i]");
    //     graph->AddEdge(nodeYi->id, nodeConvY->id, ComputeEdgeKind::DataFlow, "y[i]");
    //     outs() << "  DataFlow: x[i] -> bf16_to_fp32, y[i] -> bf16_to_fp32\n";
    //
    //     // 边5-6: conv -> mul
    //     graph->AddEdge(nodeConvX->id, nodeMul->id, ComputeEdgeKind::DataFlow, "fp32_x");
    //     graph->AddEdge(nodeConvY->id, nodeMul->id, ComputeEdgeKind::DataFlow, "fp32_y");
    //     outs() << "  DataFlow: bf16_to_fp32 -> multiply (both operands)\n";
    //
    //     // 边7: mul -> add
    //     graph->AddEdge(nodeMul->id, nodeAdd->id, ComputeEdgeKind::DataFlow, "product");
    //     outs() << "  DataFlow: multiply -> accumulate\n";
    //
    //     // 边8: sumf_in -> add (循环携带!)
    //     auto edge8 = graph->AddEdge(nodeSumfIn->id, nodeAdd->id,
    //                                  ComputeEdgeKind::LoopCarried, "sumf");
    //     outs() << "  LoopCarried: sumf_in -> accumulate  ★ 关键依赖\n";
    //
    //     // 边9: add -> store
    //     graph->AddEdge(nodeAdd->id, nodeStore->id, ComputeEdgeKind::DataFlow, "sumf_new");
    //     outs() << "  DataFlow: accumulate -> store\n";
    //
    //     // 边10: store -> sumf_in (回边)
    //     graph->AddEdge(nodeStore->id, nodeSumfIn->id, ComputeEdgeKind::LoopCarried, "sumf");
    //     outs() << "  LoopCarried: store -> sumf_in  ★ 回边\n";
    //
    //     // 统计
    //     outs() << "\n  Step 3: 计算图统计\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //     outs() << "  节点数: " << graph->NodeCount() << "\n";
    //     outs() << "  边数:   " << graph->EdgeCount() << "\n";
    //
    //     // 节点类型分布
    //     std::map<ComputeNodeKind, int> kindCount;
    //     for (const auto& node : graph->GetAllNodes()) {
    //         kindCount[node->kind]++;
    //     }
    //     outs() << "  节点分布:\n";
    //     for (const auto& [kind, count] : kindCount) {
    //         outs() << "    " << ComputeNodeKindToString(kind) << ": " << count << "\n";
    //     }
    //
    //     // 边类型分布
    //     std::map<ComputeEdgeKind, int> edgeKindCount;
    //     for (const auto& edge : graph->GetAllEdges()) {
    //         edgeKindCount[edge->kind]++;
    //     }
    //     outs() << "  边分布:\n";
    //     for (const auto& [kind, count] : edgeKindCount) {
    //         outs() << "    " << ComputeEdgeKindToString(kind) << ": " << count << "\n";
    //     }
    //
    //     // NEON映射说明
    //     outs() << "\n  Step 4: NEON 指令映射\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //     outs() << "  原始节点                      → NEON指令\n";
    //     outs() << "  ─────────────────────────────────────────────────────────────\n";
    //     outs() << "  ArrayAccess(x[i])             → vld1q_bf16(x+i)\n";
    //     outs() << "  ArrayAccess(y[i])             → vld1q_bf16(y+i)\n";
    //     outs() << "  Call(bf16_to_fp32) × 2  ┐\n";
    //     outs() << "  BinaryOp(Mul)           ├───→ vbfdotq_f32(acc, vx, vy)\n";
    //     outs() << "  BinaryOp(Add)           ┘     (4节点融合为1指令!)\n";
    //     outs() << "  水平求和                      → vaddvq_f32(acc)\n";
    //     outs() << "  累加器初始化                  → vdupq_n_f32(0.0f)\n";
    //
    //     // 生成向量化代码
    //     outs() << "\n  Step 5: 生成的向量化代码\n";
    //     outs() << "  ───────────────────────────────────────────────────────────────\n";
    //     outs() << "  float32x4_t acc = vdupq_n_f32(0.0f);\n";
    //     outs() << "  for (; i <= n - 8; i += 8) {\n";
    //     outs() << "      acc = vbfdotq_f32(acc,\n";
    //     outs() << "                        vld1q_bf16((bfloat16_t*)(x+i)),\n";
    //     outs() << "                        vld1q_bf16((bfloat16_t*)(y+i)));\n";
    //     outs() << "  }\n";
    //     outs() << "  sumf += vaddvq_f32(acc);\n";
    //
    //     // 导出DOT文件
    //     if (g_cgConfig.visualize) {
    //         std::string dotFile = g_cgConfig.outputDir + "/bf16_dot_product.dot";
    //         graph->ExportDotFile(dotFile);
    //         outs() << "\n  已生成: " << dotFile << "\n";
    //     }
    //
    //     TestResult result;
    //     result.testName = "BF16 Dot Product (Manual)";
    //     result.passed = true;
    //     result.graphCount = 1;
    //     result.nodeCount = graph->NodeCount();
    //     result.edgeCount = graph->EdgeCount();
    //     result.message = "Manual graph construction demo";
    //     results.push_back(result);
    // }

    // ========================================
    // Demo 5: 打印统计信息
    // ========================================
    void RunDemoPrintStatistics()
    {
        PrintSubHeader("Statistics Summary");

        cpgContext.PrintStatistics();

        outs() << "\nFunctions analyzed: " << functions.size() << "\n";
        for (auto* func : functions) {
            outs() << "  - " << func->getNameAsString() << "\n";
        }

        // 打印测试结果
        outs() << "\nTest Results:\n";
        int totalNodes = 0, totalEdges = 0, totalGraphs = 0;
        for (const auto& result : results) {
            outs() << "  " << (result.passed ? "✓" : "✗") << " ";
            outs() << result.testName << ": " << result.message;
            outs() << " (graphs=" << result.graphCount;
            outs() << ", nodes=" << result.nodeCount;
            outs() << ", edges=" << result.edgeCount << ")\n";
            totalGraphs += result.graphCount;
            totalNodes += result.nodeCount;
            totalEdges += result.edgeCount;
        }

        outs() << "\nTotals: " << totalGraphs << " graphs, ";
        outs() << totalNodes << " nodes, " << totalEdges << " edges\n";
    }
};

// ============================================
// AST Consumer
// ============================================
class ComputeGraphConsumer : public ASTConsumer {
public:
    explicit ComputeGraphConsumer(CompilerInstance& CI) {}

    void HandleTranslationUnit(ASTContext& context) override
    {
        // 创建CPG上下文
        cpg::CPGContext cpgContext(context);

        // 创建Demo运行器并执行
        DemoRunner runner(context, cpgContext);
        runner.RunAllDemos();
    }
};

// ============================================
// Frontend Action
// ============================================
class ComputeGraphAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(
        CompilerInstance& CI, StringRef file) override
    {
        outs() << "Analyzing file: " << file << "\n";
        return std::make_unique<ComputeGraphConsumer>(CI);
    }
};

// ============================================
// 主函数
// ============================================
int main(int argc, const char** argv)
{
    // 解析命令行参数
    auto ExpectedParser = CommonOptionsParser::create(
        argc, argv, ToolCategory, cl::ZeroOrMore,
        "Compute Graph Analysis Tool\n"
        "Analyzes C/C++ code and builds computation graphs for vectorization.\n\n"
        "Example usage:\n"
        "  ./ComputeGraphTool simple_arithmetic_test.cpp\n"
        "  ./ComputeGraphTool bf16_dot_product_test.cpp --bf16-demo\n"
        "  ./ComputeGraphTool loop_array_test.cpp --function=sum_array\n");

    if (!ExpectedParser) {
        errs() << ExpectedParser.takeError();
        return 1;
    }

    CommonOptionsParser& OptionsParser = ExpectedParser.get();

    // 检查输入文件
    if (OptionsParser.getSourcePathList().empty()) {
        errs() << "Error: No input files specified.\n";
        errs() << "Usage: ComputeGraphTool <input.cpp> [options]\n";
        return 1;
    }

    // 初始化配置
    InitializeConfig();
    PrintToolBanner();

    // 运行Clang工具
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());

    return Tool.run(newFrontendActionFactory<ComputeGraphAction>().get());
}