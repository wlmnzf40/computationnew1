/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "code_property_graph/CPGAnnotation.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace cpg {

// ============================================
// 可视化入口方法
// ============================================

void CPGContext::VisualizeICFG(const clang::FunctionDecl* func,
    const std::string& outputPath) const
{
    std::string filename = outputPath + "/" + func->getNameAsString() + "_icfg.dot";
    // 生成包含调用链的完整ICFG
    ExportICFGWithCalleesDotFile(func, filename);
    llvm::outs() << "ICFG saved to: " << filename << "\n";
}

void CPGContext::VisualizePDG(const clang::FunctionDecl* func,
    const std::string& outputPath) const
{
    std::string filename = outputPath + "/" + func->getNameAsString() + "_pdg.dot";
    ExportPDGDotFile(func, filename);
    llvm::outs() << "PDG saved to: " << filename << "\n";
}

void CPGContext::VisualizeCPG(const clang::FunctionDecl* func,
    const std::string& outputPath) const
{
    std::string filename = outputPath + "/" + func->getNameAsString() + "_cpg.dot";
    ExportCPGDotFile(func, filename);
    llvm::outs() << "CPG saved to: " << filename << "\n";
}

// ============================================
// 带调用链的 ICFG DOT导出
// ============================================

void CPGContext::CollectCalleeFunctions(
    const clang::FunctionDecl* func,
    std::set<const clang::FunctionDecl*>& collected) const
{
    auto* canonicalFunc = func->getCanonicalDecl();
    if (collected.count(canonicalFunc)) {
        return;  // 已经收集过，避免循环
    }
    collected.insert(canonicalFunc);
    llvm::outs() << "[DEBUG] CollectCalleeFunctions: collected "
                 << canonicalFunc->getNameAsString() << "\n";

    // 查找此函数中的所有调用
    auto it = icfgNodes.find(canonicalFunc);
    if (it == icfgNodes.end()) {
        llvm::outs() << "[DEBUG]   No icfgNodes for this function\n";
        return;
    }

    for (const auto& node : it->second) {
        if (node->kind == ICFGNodeKind::CallSite && node->callExpr) {
            // 从 callTargets 获取被调用函数（已经规范化）
            auto targetIt = callTargets.find(node->callExpr);
            if (targetIt != callTargets.end() && targetIt->second) {
                llvm::outs() << "[DEBUG]   Found call to: "
                             << targetIt->second->getNameAsString() << "\n";
                // 递归收集被调用函数
                CollectCalleeFunctions(targetIt->second, collected);
            }
        }
    }
}

// 辅助函数：写子图中的所有节点
void CPGContext::WriteICFGSubgraphNodes(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    auto it = icfgNodes.find(func);
    if (it == icfgNodes.end()) {
        return;
    }

    for (const auto& node : it->second) {
        auto idIt = globalNodeIds.find(node.get());
        if (idIt == globalNodeIds.end()) {
            continue;
        }

        int nodeId = idIt->second;
        out << "    n" << nodeId << " [label=\"";
        out << EscapeForDot(node->GetLabel());
        if (node->stmt) {
            out << "\\n" << EscapeForDot(GetStmtSource(node->stmt));
        }
        out << "\", style=filled, fillcolor=";
        out << GetNodeColor(node->kind) << "];\n";
    }
}

// 辅助函数：写单个函数的子图
void CPGContext::WriteICFGSubgraph(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const clang::FunctionDecl* entryFunc,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    out << "  subgraph cluster_" << func->getNameAsString() << " {\n";
    out << "    label=\"" << func->getNameAsString() << "\";\n";
    out << "    style=rounded;\n";

    if (func == entryFunc->getCanonicalDecl()) {
        out << "    bgcolor=lightcyan;\n\n";
    } else {
        out << "    bgcolor=lightyellow;\n\n";
    }

    WriteICFGSubgraphNodes(out, func, globalNodeIds);
    out << "  }\n\n";
}

// 辅助函数：写单个函数的所有边
void CPGContext::WriteICFGFunctionEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    auto it = icfgNodes.find(func);
    if (it == icfgNodes.end()) {
        return;
    }

    for (const auto& node : it->second) {
        auto fromIt = globalNodeIds.find(node.get());
        if (fromIt == globalNodeIds.end()) {
            continue;
        }

        int fromId = fromIt->second;
        for (const auto& [succ, kind] : node->successors) {
            auto toIt = globalNodeIds.find(succ);
            if (toIt == globalNodeIds.end()) {
                continue;
            }

            out << "  n" << fromId << " -> n" << toIt->second << " [";
            WriteEdgeAttributes(out, kind);
            out << "];\n";
        }
    }
}

// 主函数：导出ICFG及其调用的函数
// 注意：使用你已有的 CollectCalleeFunctions 函数
void CPGContext::ExportICFGWithCalleesDotFile(
    const clang::FunctionDecl* func,
    const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    // 收集所有需要包含的函数
    std::set<const clang::FunctionDecl*> funcsToInclude;
    CollectCalleeFunctions(func, funcsToInclude);

    // 写DOT文件头
    out << "digraph ICFG_" << func->getNameAsString() << " {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    out << "  compound=true;\n\n";

    // 为所有节点分配全局ID
    std::map<ICFGNode*, int> globalNodeIds;
    int globalId = 0;
    for (const auto* includedFunc : funcsToInclude) {
        auto it = icfgNodes.find(includedFunc);
        if (it == icfgNodes.end()) {
            continue;
        }

        for (const auto& node : it->second) {
            globalNodeIds[node.get()] = globalId++;
        }
    }

    // 写所有子图
    for (const auto* includedFunc : funcsToInclude) {
        WriteICFGSubgraph(out, includedFunc, func, globalNodeIds);
    }

    // 写所有边
    out << "  // Edges\n";
    for (const auto* includedFunc : funcsToInclude) {
        WriteICFGFunctionEdges(out, includedFunc, globalNodeIds);
    }

    out << "}\n";
}

// ============================================
// ICFG DOT导出
// ============================================

void CPGContext::ExportICFGDotFile(const clang::FunctionDecl* func,
    const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "digraph ICFG {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n\n";

    auto it = icfgNodes.find(func->getCanonicalDecl());
    if (it == icfgNodes.end()) {
        return;
    }

    std::map<ICFGNode*, int> nodeIds;
    WriteICFGNodes(out, func, nodeIds);
    WriteICFGEdges(out, func, nodeIds);

    out << "}\n";
}

void CPGContext::WriteICFGNodes(llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func, std::map<ICFGNode*, int>& nodeIds) const
{
    auto it = icfgNodes.find(func->getCanonicalDecl());
    if (it == icfgNodes.end()) {
        return;
    }

    int id = 0;
    for (const auto& node : it->second) {
        nodeIds[node.get()] = id;

        out << "  n" << id << " [label=\"";
        out << EscapeForDot(node->GetLabel());

        if (node->stmt) {
            out << "\\n" << EscapeForDot(GetStmtSource(node->stmt));
        }

        out << "\", style=filled, fillcolor=";
        out << GetNodeColor(node->kind);
        out << "];\n";

        id++;
    }
}

std::string CPGContext::GetNodeColor(ICFGNodeKind kind) const
{
    switch (kind) {
        case ICFGNodeKind::Entry: return "lightgreen";
        case ICFGNodeKind::Exit: return "lightblue";
        case ICFGNodeKind::CallSite: return "yellow";
        case ICFGNodeKind::ReturnSite: return "orange";
        case ICFGNodeKind::ActualIn: return "lightsalmon";    // 实参入
        case ICFGNodeKind::ActualOut: return "lightcoral";    // 实参出
        case ICFGNodeKind::FormalIn: return "palegreen";      // 形参入
        case ICFGNodeKind::FormalOut: return "darkseagreen";  // 形参出
        default: return "white";
    }
}

void CPGContext::WriteICFGEdges(llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& nodeIds) const
{
    auto it = icfgNodes.find(func->getCanonicalDecl());
    if (it == icfgNodes.end()) {
        return;
    }

    out << "\n";
    for (const auto& node : it->second) {
        int fromId = nodeIds.at(node.get());

        for (const auto& [succ, kind] : node->successors) {
            auto succIt = nodeIds.find(succ);
            if (succIt != nodeIds.end()) {
                int toId = succIt->second;
                out << "  n" << fromId << " -> n" << toId << " [";
                WriteEdgeAttributes(out, kind);
                out << "];\n";
            }
        }
    }
}

void CPGContext::WriteEdgeAttributes(llvm::raw_fd_ostream& out,
    ICFGEdgeKind kind) const
{
    switch (kind) {
        case ICFGEdgeKind::Call:
            out << "label=\"call\", color=red, style=bold";
            break;
        case ICFGEdgeKind::Return:
            out << "label=\"ret\", color=blue, style=dashed";
            break;
        case ICFGEdgeKind::ParamIn:
            out << "label=\"param_in\", color=purple, style=dotted";
            break;
        case ICFGEdgeKind::ParamOut:
            out << "label=\"param_out\", color=magenta, style=dotted";
            break;
        case ICFGEdgeKind::True:
            out << "label=\"T\", color=green";
            break;
        case ICFGEdgeKind::False:
            out << "label=\"F\", color=red";
            break;
        default:
            out << "color=black";
            break;
    }
}

// ============================================
// PDG DOT导出
// ============================================

void CPGContext::ExportPDGDotFile(const clang::FunctionDecl* func,
    const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "digraph PDG {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n\n";

    std::map<const clang::Stmt*, int> nodeIds;
    WritePDGNodes(out, func, nodeIds);
    WritePDGEdges(out, func, nodeIds);

    out << "}\n";
}

void CPGContext::WritePDGNodes(llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func, std::map<const clang::Stmt*, int>& nodeIds) const
{
    int id = 0;
    for (const auto& [stmt, node] : pdgNodes) {
        if (GetContainingFunction(stmt) != func) {
            continue;
        }

        nodeIds[stmt] = id;
        out << "  n" << id << " [label=\"";
        out << EscapeForDot(GetStmtSource(stmt));
        out << "\"];\n";
        id++;
    }
}

void CPGContext::WritePDGEdges(llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<const clang::Stmt*, int>& nodeIds) const
{
    WritePDGDataEdges(out, func, nodeIds);
    WritePDGControlEdges(out, func, nodeIds);
}

void CPGContext::WritePDGDataEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<const clang::Stmt*, int>& nodeIds) const
{
    out << "\n  // Data dependencies\n";
    for (const auto& [stmt, node] : pdgNodes) {
        if (GetContainingFunction(stmt) != func) {
            continue;
        }

        auto stmtIt = nodeIds.find(stmt);
        if (stmtIt == nodeIds.end()) {
            continue;
        }
        int toId = stmtIt->second;

        for (const auto& dep : node->dataDeps) {
            auto srcIt = nodeIds.find(dep.sourceStmt);
            if (srcIt != nodeIds.end()) {
                int fromId = srcIt->second;
                out << "  n" << fromId << " -> n" << toId
                    << " [label=\"" << EscapeForDot(dep.varName)
                    << "\", color=blue, style=dashed];\n";
            }
        }
    }
}

void CPGContext::WritePDGControlEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<const clang::Stmt*, int>& nodeIds) const
    {
    out << "\n  // Control dependencies\n";
    for (const auto& [stmt, node] : pdgNodes) {
        if (GetContainingFunction(stmt) != func) {
            continue;
        }

        auto stmtIt = nodeIds.find(stmt);
        if (stmtIt == nodeIds.end()) {
            continue;
        }
        int toId = stmtIt->second;

        for (const auto& dep : node->controlDeps) {
            auto ctrlIt = nodeIds.find(dep.controlStmt);
            if (ctrlIt != nodeIds.end()) {
                int fromId = ctrlIt->second;
                out << "  n" << fromId << " -> n" << toId
                    << " [label=\"" << (dep.branchValue ? "T" : "F")
                    << "\", color=red, style=dotted];\n";
            }
        }
    }
}

// ============================================
// CPG DOT导出 (ICFG + PDG)
// ============================================

void CPGContext::BuildCPGNodeMappings(
    const clang::FunctionDecl* func,
    std::map<ICFGNode*, int>& icfgNodeIds,
    std::map<const clang::Stmt*, int>& stmtToNodeId) const
{
    auto icfgIt = icfgNodes.find(func->getCanonicalDecl());
    if (icfgIt == icfgNodes.end()) {
        return;
    }

    int id = 0;
    for (const auto& node : icfgIt->second) {
        icfgNodeIds[node.get()] = id;
        if (node->stmt) {
            stmtToNodeId[node->stmt] = id;
        }
        id++;
    }
}

void CPGContext::WriteCPGNodes(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& icfgNodeIds) const
{
    auto icfgIt = icfgNodes.find(func->getCanonicalDecl());
    if (icfgIt == icfgNodes.end()) {
        return;
    }

    out << "  // ICFG Nodes\n";
    for (const auto& node : icfgIt->second) {
        int nodeId = icfgNodeIds.at(node.get());
        out << "  n" << nodeId << " [label=\"";
        out << EscapeForDot(node->GetLabel());
        if (node->stmt) {
            out << "\\n" << EscapeForDot(GetStmtSource(node->stmt));
        }
        out << "\", style=filled, fillcolor=" << GetNodeColor(node->kind);
        out << "];\n";
    }
}

void CPGContext::WriteCPGControlFlowEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& icfgNodeIds) const
{
    auto icfgIt = icfgNodes.find(func->getCanonicalDecl());
    if (icfgIt == icfgNodes.end()) {
        return;
    }

    out << "\n  // Control Flow Edges (ICFG)\n";
    for (const auto& node : icfgIt->second) {
        int fromId = icfgNodeIds.at(node.get());
        for (const auto& [succ, kind] : node->successors) {
            auto succIt = icfgNodeIds.find(succ);
            if (succIt != icfgNodeIds.end()) {
                out << "  n" << fromId << " -> n" << succIt->second << " [";
                WriteEdgeAttributes(out, kind);
                out << "];\n";
            }
        }
    }
}

void CPGContext::WriteCPGDataDepEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<const clang::Stmt*, int>& stmtToNodeId) const
{
    out << "\n  // Data Dependency Edges (PDG)\n";
    for (const auto& [stmt, pdgNode] : pdgNodes) {
        if (GetContainingFunction(stmt) != func) {
            continue;
        }
        auto sinkIt = stmtToNodeId.find(stmt);
        if (sinkIt == stmtToNodeId.end()) {
            continue;
        }

        for (const auto& dep : pdgNode->dataDeps) {
            auto srcIt = stmtToNodeId.find(dep.sourceStmt);
            if (srcIt != stmtToNodeId.end()) {
                out << "  n" << srcIt->second << " -> n" << sinkIt->second
                    << " [label=\"" << EscapeForDot(dep.varName)
                    << "\", color=blue, style=dashed, constraint=false];\n";
            }
        }
    }
}

void CPGContext::WriteCPGControlDepEdges(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<const clang::Stmt*, int>& stmtToNodeId) const
{
    out << "\n  // Control Dependency Edges (PDG)\n";
    for (const auto& [stmt, pdgNode] : pdgNodes) {
        if (GetContainingFunction(stmt) != func) {
            continue;
        }
        auto depIt = stmtToNodeId.find(stmt);
        if (depIt == stmtToNodeId.end()) {
            continue;
        }

        for (const auto& dep : pdgNode->controlDeps) {
            auto ctrlIt = stmtToNodeId.find(dep.controlStmt);
            if (ctrlIt != stmtToNodeId.end()) {
                out << "  n" << ctrlIt->second << " -> n" << depIt->second
                    << " [label=\"" << (dep.branchValue ? "T" : "F")
                    << "\", color=red, style=dotted, constraint=false];\n";
            }
        }
    }
}

// 辅助函数：为CPG分配全局节点ID（包括stmt映射）
void CPGContext::AssignGlobalNodeIdsForCPG(
    const std::set<const clang::FunctionDecl*>& funcsToInclude,
    std::map<ICFGNode*, int>& globalNodeIds,
    std::map<const clang::Stmt*, int>& globalStmtToNodeId) const
{
    int globalId = 0;

    for (const auto* includedFunc : funcsToInclude) {
        auto it = icfgNodes.find(includedFunc);
        if (it == icfgNodes.end()) {
            continue;
        }

        for (const auto& node : it->second) {
            globalNodeIds[node.get()] = globalId;
            if (node->stmt) {
                globalStmtToNodeId[node->stmt] = globalId;
            }
            globalId++;
        }
    }
}

// 辅助函数：写CPG子图中的所有节点
void CPGContext::WriteCPGSubgraphNodes(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    auto it = icfgNodes.find(func);
    if (it == icfgNodes.end()) {
        return;
    }

    for (const auto& node : it->second) {
        auto idIt = globalNodeIds.find(node.get());
        if (idIt == globalNodeIds.end()) {
            continue;
        }

        int nodeId = idIt->second;
        out << "    n" << nodeId << " [label=\"";
        out << EscapeForDot(node->GetLabel());
        if (node->stmt) {
            out << "\\n" << EscapeForDot(GetStmtSource(node->stmt));
        }
        out << "\", style=filled, fillcolor=";
        out << GetNodeColor(node->kind) << "];\n";
    }
}

// 辅助函数：写单个CPG子图（包括头部和节点）
void CPGContext::WriteCPGSubgraph(
    llvm::raw_fd_ostream& out,
    const clang::FunctionDecl* func,
    const clang::FunctionDecl* entryFunc,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    out << "  subgraph cluster_" << func->getNameAsString() << " {\n";
    out << "    label=\"" << func->getNameAsString() << "\";\n";
    out << "    style=rounded;\n";

    if (func == entryFunc->getCanonicalDecl()) {
        out << "    bgcolor=lightcyan;\n\n";
    } else {
        out << "    bgcolor=lightyellow;\n\n";
    }

    WriteCPGSubgraphNodes(out, func, globalNodeIds);
    out << "  }\n\n";
}

// 辅助函数：写单个节点的控制流边
void CPGContext::WriteNodeControlFlowEdges(
    llvm::raw_fd_ostream& out,
    ICFGNode* node,
    int fromId,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    for (const auto& [succ, kind] : node->successors) {
        auto toIt = globalNodeIds.find(succ);
        if (toIt == globalNodeIds.end()) {
            continue;
        }

        out << "  n" << fromId << " -> n" << toIt->second << " [";
        WriteEdgeAttributes(out, kind);
        out << "];\n";
    }
}

// 辅助函数：写CPG控制流边
void CPGContext::WriteCPGControlFlowEdges(
    llvm::raw_fd_ostream& out,
    const std::set<const clang::FunctionDecl*>& funcsToInclude,
    const std::map<ICFGNode*, int>& globalNodeIds) const
{
    out << "  // Control Flow Edges\n";

    for (const auto* includedFunc : funcsToInclude) {
        auto it = icfgNodes.find(includedFunc);
        if (it == icfgNodes.end()) {
            continue;
        }

        for (const auto& node : it->second) {
            auto fromIt = globalNodeIds.find(node.get());
            if (fromIt == globalNodeIds.end()) {
                continue;
            }

            WriteNodeControlFlowEdges(out, node.get(), fromIt->second, globalNodeIds);
        }
    }
}

// 辅助函数：写单个PDG节点的数据依赖边
void CPGContext::WritePDGNodeDataDeps(
    llvm::raw_fd_ostream& out,
    const PDGNode* pdgNode,
    int sinkId,
    const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const
{
    for (const auto& dep : pdgNode->dataDeps) {
        auto srcIt = globalStmtToNodeId.find(dep.sourceStmt);
        if (srcIt != globalStmtToNodeId.end()) {
            out << "  n" << srcIt->second << " -> n" << sinkId
                << " [label=\"" << EscapeForDot(dep.varName)
                << "\", color=blue, style=dashed, constraint=false];\n";
        }
    }
}

// 辅助函数：写CPG数据依赖边
void CPGContext::WriteCPGDataDependencyEdges(
    llvm::raw_fd_ostream& out,
    const std::set<const clang::FunctionDecl*>& funcsToInclude,
    const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const
{
    out << "\n  // Data Dependency Edges\n";

    for (const auto* includedFunc : funcsToInclude) {
        for (const auto& [stmt, pdgNode] : pdgNodes) {
            if (GetContainingFunction(stmt) != includedFunc) {
                continue;
            }

            auto sinkIt = globalStmtToNodeId.find(stmt);
            if (sinkIt == globalStmtToNodeId.end()) {
                continue;
            }

            WritePDGNodeDataDeps(out, pdgNode.get(), sinkIt->second, globalStmtToNodeId);
        }
    }
}

// 辅助函数：写单个PDG节点的控制依赖边
void CPGContext::WritePDGNodeControlDeps(
    llvm::raw_fd_ostream& out,
    const PDGNode* pdgNode,
    int depId,
    const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const
{
    for (const auto& dep : pdgNode->controlDeps) {
        auto ctrlIt = globalStmtToNodeId.find(dep.controlStmt);
        if (ctrlIt != globalStmtToNodeId.end()) {
            out << "  n" << ctrlIt->second << " -> n" << depId
                << " [label=\"" << (dep.branchValue ? "T" : "F")
                << "\", color=red, style=dotted, constraint=false];\n";
        }
    }
}

// 辅助函数：写CPG控制依赖边
void CPGContext::WriteCPGControlDependencyEdges(
    llvm::raw_fd_ostream& out,
    const std::set<const clang::FunctionDecl*>& funcsToInclude,
    const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const
{
    out << "\n  // Control Dependency Edges\n";

    for (const auto* includedFunc : funcsToInclude) {
        for (const auto& [stmt, pdgNode] : pdgNodes) {
            if (GetContainingFunction(stmt) != includedFunc) {
                continue;
            }

            auto depIt = globalStmtToNodeId.find(stmt);
            if (depIt == globalStmtToNodeId.end()) {
                continue;
            }

            WritePDGNodeControlDeps(out, pdgNode.get(), depIt->second, globalStmtToNodeId);
        }
    }
}

// 主函数：导出CPG（ICFG + PDG）
void CPGContext::ExportCPGDotFile(const clang::FunctionDecl* func,
    const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    // 收集入口函数及其所有被调用函数
    std::set<const clang::FunctionDecl*> funcsToInclude;
    CollectCalleeFunctions(func, funcsToInclude);

    // 写DOT文件头
    out << "digraph CPG_" << func->getNameAsString() << " {\n";
    out << "  rankdir=TB;\n";
    out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    out << "  compound=true;\n\n";

    // 为所有函数的节点分配全局ID
    std::map<ICFGNode*, int> globalNodeIds;
    std::map<const clang::Stmt*, int> globalStmtToNodeId;
    AssignGlobalNodeIdsForCPG(funcsToInclude, globalNodeIds, globalStmtToNodeId);

    // 写所有子图
    for (const auto* includedFunc : funcsToInclude) {
        WriteCPGSubgraph(out, includedFunc, func, globalNodeIds);
    }

    // 写控制流边
    WriteCPGControlFlowEdges(out, funcsToInclude, globalNodeIds);

    // 写数据依赖边
    WriteCPGDataDependencyEdges(out, funcsToInclude, globalStmtToNodeId);

    // 写控制依赖边
    WriteCPGControlDependencyEdges(out, funcsToInclude, globalStmtToNodeId);

    out << "}\n";
}

} // namespace cpg