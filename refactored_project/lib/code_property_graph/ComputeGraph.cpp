/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraph.cpp - 计算图抽象层实现
 */
#include "ComputeGraph.h"
#include "code_property_graph/CPGAnnotation.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <queue>
#include <stack>
#include <algorithm>
#include <sstream>

namespace compute_graph {

// ============================================
// DataTypeInfo 实现
// ============================================

std::string DataTypeInfo::ToString() const
{
    std::ostringstream oss;
    switch (baseType) {
        case BaseType::Int8: oss << "i8"; break;
        case BaseType::Int16: oss << "i16"; break;
        case BaseType::Int32: oss << "i32"; break;
        case BaseType::Int64: oss << "i64"; break;
        case BaseType::UInt8: oss << "u8"; break;
        case BaseType::UInt16: oss << "u16"; break;
        case BaseType::UInt32: oss << "u32"; break;
        case BaseType::UInt64: oss << "u64"; break;
        case BaseType::Float: oss << "f32"; break;
        case BaseType::Double: oss << "f64"; break;
        case BaseType::Pointer: oss << "ptr"; break;
        case BaseType::Array: oss << "arr"; break;
        case BaseType::Void: oss << "void"; break;
        case BaseType::TemplateParam:
            // 显示模板参数名（如 T）
            oss << (typeName.empty() ? "T" : typeName);
            break;
        case BaseType::Dependent:
            // 显示依赖类型名
            oss << (typeName.empty() ? "<dependent>" : typeName);
            break;
        default: oss << "unknown"; break;
    }
    if (vectorWidth > 1) {
        oss << "x" << vectorWidth;
    }
    return oss.str();
}

DataTypeInfo DataTypeInfo::FromClangType(const clang::QualType& type)
{
    DataTypeInfo info;

    if (type.isNull()) {
        return info;
    }

    const clang::Type* typePtr = type.getTypePtr();

    // 【改进】处理依赖类型（模板参数）
    if (typePtr->isDependentType()) {
        // 检查是否是模板类型参数（如 T, U 等用户定义的类型名）
        if (auto* templateParam = typePtr->getAs<clang::TemplateTypeParmType>()) {
            info.baseType = BaseType::TemplateParam;
            if (auto* decl = templateParam->getDecl()) {
                info.typeName = decl->getNameAsString();
            } else {
                // 如果没有声明，尝试从类型字符串获取
                info.typeName = type.getAsString();
            }
        } else {
            // 其他依赖类型 - 尝试获取可读的类型名
            std::string typeStr = type.getAsString();

            // 如果是 "<dependent type>" 这种不可读的，尝试简化
            if (typeStr.find("<dependent") != std::string::npos ||
                typeStr.find("dependent") != std::string::npos) {
                // 检查是否是 const T * 之类的
                if (typePtr->isPointerType()) {
                    info.baseType = BaseType::Pointer;
                    info.typeName = "T*";  // 模板指针
                } else {
                    info.baseType = BaseType::Dependent;
                    info.typeName = "<T>";  // 标记为模板相关
                }
            } else {
                info.baseType = BaseType::Dependent;
                info.typeName = typeStr;
            }
        }
        return info;
    }

    if (typePtr->isPointerType()) {
        info.baseType = BaseType::Pointer;
        info.bitWidth = 64;  // 假设64位系统
    } else if (typePtr->isArrayType()) {
        info.baseType = BaseType::Array;
    } else if (typePtr->isFloatingType()) {
        if (typePtr->isFloat128Type() || typePtr->isSpecificBuiltinType(clang::BuiltinType::Double)) {
            info.baseType = BaseType::Double;
            info.bitWidth = 64;
        } else {
            info.baseType = BaseType::Float;
            info.bitWidth = 32;
        }
        info.isSigned = true;
    } else if (typePtr->isIntegerType()) {
        info.isSigned = typePtr->isSignedIntegerType();

        if (auto* builtinType = typePtr->getAs<clang::BuiltinType>()) {
            switch (builtinType->getKind()) {
                case clang::BuiltinType::Char_S:
                case clang::BuiltinType::SChar:
                    info.baseType = BaseType::Int8;
                    info.bitWidth = 8;
                    break;
                case clang::BuiltinType::Char_U:
                case clang::BuiltinType::UChar:
                    info.baseType = BaseType::UInt8;
                    info.bitWidth = 8;
                    break;
                case clang::BuiltinType::Short:
                    info.baseType = BaseType::Int16;
                    info.bitWidth = 16;
                    break;
                case clang::BuiltinType::UShort:
                    info.baseType = BaseType::UInt16;
                    info.bitWidth = 16;
                    break;
                case clang::BuiltinType::Int:
                    info.baseType = BaseType::Int32;
                    info.bitWidth = 32;
                    break;
                case clang::BuiltinType::UInt:
                    info.baseType = BaseType::UInt32;
                    info.bitWidth = 32;
                    break;
                case clang::BuiltinType::Long:
                case clang::BuiltinType::LongLong:
                    info.baseType = BaseType::Int64;
                    info.bitWidth = 64;
                    break;
                case clang::BuiltinType::ULong:
                case clang::BuiltinType::ULongLong:
                    info.baseType = BaseType::UInt64;
                    info.bitWidth = 64;
                    break;
                default:
                    info.baseType = BaseType::Int32;
                    info.bitWidth = 32;
                    break;
            }
        }
    } else if (typePtr->isVoidType()) {
        info.baseType = BaseType::Void;
    }

    return info;
}

// ============================================
// ComputeNode 实现
// ============================================

ComputeNode::ComputeNode(ComputeNodeKind k, NodeId nodeId)
    : kind(k), id(nodeId)
{
    constValue.intValue = 0;
}

std::string ComputeNode::GetLabel() const
{
    std::ostringstream oss;
    oss << GetKindName();

    if (!name.empty()) {
        oss << ": " << name;
    }

    if (kind == ComputeNodeKind::BinaryOp ||
        kind == ComputeNodeKind::UnaryOp ||
        kind == ComputeNodeKind::CompareOp) {
        oss << " [" << OpCodeToString(opCode) << "]";
    }

    if (hasConstValue) {
        if (dataType.baseType == DataTypeInfo::BaseType::Float ||
            dataType.baseType == DataTypeInfo::BaseType::Double) {
            oss << " = " << constValue.floatValue;
        } else {
            oss << " = " << constValue.intValue;
        }
    }

    return oss.str();
}

std::string ComputeNode::GetKindName() const
{
    return ComputeNodeKindToString(kind);
}

void ComputeNode::SetProperty(const std::string& key, const std::string& value)
{
    properties[key] = value;
}

std::string ComputeNode::GetProperty(const std::string& key) const
{
    auto it = properties.find(key);
    return it != properties.end() ? it->second : "";
}

bool ComputeNode::HasProperty(const std::string& key) const
{
    return properties.find(key) != properties.end();
}

bool ComputeNode::IsVectorizable() const
{
    switch (kind) {
        case ComputeNodeKind::BinaryOp:
        case ComputeNodeKind::UnaryOp:
        case ComputeNodeKind::CompareOp:
        case ComputeNodeKind::Load:
        case ComputeNodeKind::Store:
        case ComputeNodeKind::ArrayAccess:
        case ComputeNodeKind::Cast:
            return true;
        case ComputeNodeKind::Call:
            // 某些内置函数可以向量化
            return HasProperty("vectorizable");
        default:
            return false;
    }
}

bool ComputeNode::IsOperationNode() const
{
    return kind == ComputeNodeKind::BinaryOp ||
           kind == ComputeNodeKind::UnaryOp ||
           kind == ComputeNodeKind::CompareOp;
}

bool ComputeNode::IsMemoryNode() const
{
    return kind == ComputeNodeKind::Load ||
           kind == ComputeNodeKind::Store ||
           kind == ComputeNodeKind::ArrayAccess;
}

void ComputeNode::Dump() const
{
    llvm::outs() << "[Node " << id << "] " << GetLabel();
    llvm::outs() << " Type: " << dataType.ToString();
    if (loopDepth > 0) {
        llvm::outs() << " LoopDepth: " << loopDepth;
    }
    if (sourceLine > 0) {
        llvm::outs() << " L" << sourceLine;
    }
    llvm::outs() << "\n";

    if (!sourceText.empty()) {
        llvm::outs() << "  Code: " << sourceText << "\n";
    }

    if (!inputNodes.empty()) {
        llvm::outs() << "  Inputs: ";
        for (auto inId : inputNodes) {
            llvm::outs() << inId << " ";
        }
        llvm::outs() << "\n";
    }

    if (!outputNodes.empty()) {
        llvm::outs() << "  Outputs: ";
        for (auto outId : outputNodes) {
            llvm::outs() << outId << " ";
        }
        llvm::outs() << "\n";
    }
}

// ============================================
// ComputeEdge 实现
// ============================================

ComputeEdge::ComputeEdge(EdgeId edgeId, ComputeEdgeKind k,
                         ComputeNode::NodeId src, ComputeNode::NodeId tgt)
    : id(edgeId), kind(k), sourceId(src), targetId(tgt)
{}

std::string ComputeEdge::GetLabel() const
{
    std::ostringstream oss;
    oss << GetKindName();
    if (!label.empty()) {
        oss << ": " << label;
    }
    return oss.str();
}

std::string ComputeEdge::GetKindName() const
{
    return ComputeEdgeKindToString(kind);
}

// ============================================
// AnchorPoint 实现
// ============================================

std::string AnchorPoint::ToString() const
{
    std::ostringstream oss;
    oss << "Anchor[L" << sourceLine << " ";
    oss << ComputeNodeKindToString(expectedKind);
    if (opCode != OpCode::Unknown) {
        oss << "(" << OpCodeToString(opCode) << ")";
    }
    oss << " depth=" << loopDepth;
    oss << " score=" << score;
    if (!sourceText.empty()) {
        oss << " code=\"" << sourceText << "\"";
    }
    oss << "]";
    return oss.str();
}

// ============================================
// ComputeGraph 实现
// ============================================

ComputeGraph::ComputeGraph(const std::string& graphName)
    : name(graphName)
{}

ComputeGraph::NodePtr ComputeGraph::CreateNode(ComputeNodeKind kind)
{
    auto node = std::make_shared<ComputeNode>(kind, nextNodeId++);
    nodes[node->id] = node;
    return node;
}

ComputeGraph::NodePtr ComputeGraph::GetNode(ComputeNode::NodeId id) const
{
    auto it = nodes.find(id);
    return it != nodes.end() ? it->second : nullptr;
}

ComputeGraph::NodePtr ComputeGraph::FindNodeByStmt(const clang::Stmt* stmt) const
{
    auto it = stmtToNode.find(stmt);
    if (it != stmtToNode.end()) {
        return GetNode(it->second);
    }
    return nullptr;
}

ComputeGraph::NodePtr ComputeGraph::FindNodeByName(const std::string& nodeName) const
{
    auto it = nameToNode.find(nodeName);
    if (it != nameToNode.end()) {
        return GetNode(it->second);
    }
    return nullptr;
}

void ComputeGraph::RemoveNode(ComputeNode::NodeId id)
{
    auto nodeIt = nodes.find(id);
    if (nodeIt == nodes.end()) {
        return;
    }

    // 移除相关的边
    std::vector<ComputeEdge::EdgeId> edgesToRemove;
    for (const auto& [edgeId, edge] : edges) {
        if (edge->sourceId == id || edge->targetId == id) {
            edgesToRemove.push_back(edgeId);
        }
    }

    for (auto edgeId : edgesToRemove) {
        RemoveEdge(edgeId);
    }

    // 从映射中移除
    if (nodeIt->second->astStmt) {
        stmtToNode.erase(nodeIt->second->astStmt);
    }
    if (!nodeIt->second->name.empty()) {
        nameToNode.erase(nodeIt->second->name);
    }

    nodes.erase(nodeIt);
}

ComputeGraph::EdgePtr ComputeGraph::AddEdge(
    ComputeNode::NodeId src, ComputeNode::NodeId tgt,
    ComputeEdgeKind kind, const std::string& varName)
{
    auto edge = std::make_shared<ComputeEdge>(nextEdgeId++, kind, src, tgt);
    edge->label = varName;
    edges[edge->id] = edge;

    UpdateAdjacencyLists(edge);

    // 更新节点的输入输出列表
    if (auto srcNode = GetNode(src)) {
        srcNode->outputNodes.push_back(tgt);
    }
    if (auto tgtNode = GetNode(tgt)) {
        tgtNode->inputNodes.push_back(src);
    }

    return edge;
}

ComputeGraph::EdgePtr ComputeGraph::GetEdge(ComputeEdge::EdgeId id) const
{
    auto it = edges.find(id);
    return it != edges.end() ? it->second : nullptr;
}

void ComputeGraph::RemoveEdge(ComputeEdge::EdgeId id)
{
    auto edgeIt = edges.find(id);
    if (edgeIt == edges.end()) {
        return;
    }

    auto edge = edgeIt->second;

    // 更新邻接表
    auto& srcOutEdges = outEdges[edge->sourceId];
    srcOutEdges.erase(
        std::remove(srcOutEdges.begin(), srcOutEdges.end(), id),
        srcOutEdges.end());

    auto& tgtInEdges = inEdges[edge->targetId];
    tgtInEdges.erase(
        std::remove(tgtInEdges.begin(), tgtInEdges.end(), id),
        tgtInEdges.end());

    edges.erase(edgeIt);
}

std::vector<ComputeGraph::EdgePtr> ComputeGraph::GetIncomingEdges(
    ComputeNode::NodeId nodeId) const
{
    std::vector<EdgePtr> result;
    auto it = inEdges.find(nodeId);
    if (it != inEdges.end()) {
        for (auto edgeId : it->second) {
            if (auto edge = GetEdge(edgeId)) {
                result.push_back(edge);
            }
        }
    }
    return result;
}

std::vector<ComputeGraph::EdgePtr> ComputeGraph::GetOutgoingEdges(
    ComputeNode::NodeId nodeId) const
{
    std::vector<EdgePtr> result;
    auto it = outEdges.find(nodeId);
    if (it != outEdges.end()) {
        for (auto edgeId : it->second) {
            if (auto edge = GetEdge(edgeId)) {
                result.push_back(edge);
            }
        }
    }
    return result;
}

std::vector<ComputeGraph::NodePtr> ComputeGraph::GetAllNodes() const
{
    std::vector<NodePtr> result;
    result.reserve(nodes.size());
    for (const auto& [id, node] : nodes) {
        result.push_back(node);
    }
    return result;
}

std::vector<ComputeGraph::EdgePtr> ComputeGraph::GetAllEdges() const
{
    std::vector<EdgePtr> result;
    result.reserve(edges.size());
    for (const auto& [id, edge] : edges) {
        result.push_back(edge);
    }
    return result;
}

std::vector<ComputeGraph::NodePtr> ComputeGraph::GetRootNodes() const
{
    std::vector<NodePtr> result;
    for (const auto& [id, node] : nodes) {
        if (node->inputNodes.empty()) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<ComputeGraph::NodePtr> ComputeGraph::GetLeafNodes() const
{
    std::vector<NodePtr> result;
    for (const auto& [id, node] : nodes) {
        if (node->outputNodes.empty()) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<ComputeGraph::NodePtr> ComputeGraph::TopologicalSort() const
{
    std::vector<NodePtr> result;
    std::map<ComputeNode::NodeId, int> inDegree;

    // 初始化入度
    for (const auto& [id, node] : nodes) {
        inDegree[id] = node->inputNodes.size();
    }

    // BFS
    std::queue<ComputeNode::NodeId> queue;
    for (const auto& [id, degree] : inDegree) {
        if (degree == 0) {
            queue.push(id);
        }
    }

    while (!queue.empty()) {
        auto nodeId = queue.front();
        queue.pop();

        if (auto node = GetNode(nodeId)) {
            result.push_back(node);

            for (auto outId : node->outputNodes) {
                if (--inDegree[outId] == 0) {
                    queue.push(outId);
                }
            }
        }
    }

    return result;
}

    void ComputeGraph::Merge(const ComputeGraph& other)
{
    std::map<ComputeNode::NodeId, ComputeNode::NodeId> idMapping;

    // 复制节点
    for (const auto& [oldId, node] : other.nodes) {
        auto newNode = CreateNode(node->kind);
        newNode->name = node->name;
        newNode->dataType = node->dataType;
        newNode->astStmt = node->astStmt;
        newNode->astDecl = node->astDecl;
        newNode->containingFunc = node->containingFunc;
        newNode->opCode = node->opCode;
        newNode->constValue = node->constValue;
        newNode->hasConstValue = node->hasConstValue;
        newNode->properties = node->properties;
        newNode->loopDepth = node->loopDepth;
        newNode->isLoopInvariant = node->isLoopInvariant;

        // 【核心修复】复制循环和分支上下文信息
        newNode->loopContextId = node->loopContextId;
        newNode->loopContextVar = node->loopContextVar;
        newNode->loopContextLine = node->loopContextLine;
        newNode->branchContextId = node->branchContextId;
        newNode->branchType = node->branchType;
        newNode->branchContextLine = node->branchContextLine;

        idMapping[oldId] = newNode->id;
    }

    // 复制边
    for (const auto& [edgeId, edge] : other.edges) {
        auto newSrcId = idMapping[edge->sourceId];
        auto newTgtId = idMapping[edge->targetId];
        auto newEdge = AddEdge(newSrcId, newTgtId, edge->kind, edge->label);
        newEdge->weight = edge->weight;
        newEdge->properties = edge->properties;
    }
}

    ComputeGraph ComputeGraph::ExtractSubgraph(
        const std::set<ComputeNode::NodeId>& nodeIds) const
{
    ComputeGraph subgraph(name + "_sub");
    std::map<ComputeNode::NodeId, ComputeNode::NodeId> idMapping;

    // 复制指定节点
    for (auto nodeId : nodeIds) {
        if (auto node = GetNode(nodeId)) {
            auto newNode = subgraph.CreateNode(node->kind);
            newNode->name = node->name;
            newNode->dataType = node->dataType;
            newNode->astStmt = node->astStmt;
            newNode->astDecl = node->astDecl; // 补全 Decl
            newNode->containingFunc = node->containingFunc; // 补全 Func
            newNode->opCode = node->opCode;
            newNode->properties = node->properties;
            newNode->hasConstValue = node->hasConstValue;
            newNode->constValue = node->constValue;
            newNode->loopDepth = node->loopDepth;
            newNode->isLoopInvariant = node->isLoopInvariant;

            // 【核心修复】复制循环和分支上下文信息
            newNode->loopContextId = node->loopContextId; // 注意：如果是子图，这个ID可能需要重映射，但保留原值通常用于调试
            newNode->loopContextVar = node->loopContextVar;
            newNode->loopContextLine = node->loopContextLine;
            newNode->branchContextId = node->branchContextId;
            newNode->branchType = node->branchType;
            newNode->branchContextLine = node->branchContextLine;

            idMapping[nodeId] = newNode->id;
        }
    }

    // 复制边 (只复制两端都在子图中的边)
    for (const auto& [edgeId, edge] : edges) {
        if (nodeIds.count(edge->sourceId) && nodeIds.count(edge->targetId)) {
            auto newSrcId = idMapping[edge->sourceId];
            auto newTgtId = idMapping[edge->targetId];
            auto newEdge = subgraph.AddEdge(newSrcId, newTgtId, edge->kind, edge->label);
            newEdge->properties = edge->properties; // 补全属性复制
        }
    }

    return subgraph;
}

ComputeGraph ComputeGraph::Clone() const
{
    std::set<ComputeNode::NodeId> allIds;
    for (const auto& [id, _] : nodes) {
        allIds.insert(id);
    }

    auto cloned = ExtractSubgraph(allIds);
    cloned.name = name + "_clone";
    return cloned;
}

void ComputeGraph::Clear()
{
    nodes.clear();
    edges.clear();
    stmtToNode.clear();
    nameToNode.clear();
    inEdges.clear();
    outEdges.clear();
    nextNodeId = 0;
    nextEdgeId = 0;
}

std::string ComputeGraph::ComputeCanonicalSignature() const
{
    // 简化的规范化签名计算
    // 基于拓扑排序后的节点类型和边的模式
    std::ostringstream oss;

    auto sortedNodes = TopologicalSort();
    for (const auto& node : sortedNodes) {
        oss << static_cast<int>(node->kind);
        if (node->opCode != OpCode::Unknown) {
            oss << ":" << static_cast<int>(node->opCode);
        }
        oss << ";";
    }

    oss << "|";

    // 边信息
    for (const auto& [id, edge] : edges) {
        oss << edge->sourceId << "->" << edge->targetId;
        oss << ":" << static_cast<int>(edge->kind) << ";";
    }

    return oss.str();
}

bool ComputeGraph::IsIsomorphicTo(const ComputeGraph& other) const
{
    // 简化的同构检测：比较规范化签名
    return ComputeCanonicalSignature() == other.ComputeCanonicalSignature();
}

void ComputeGraph::ExportDotFile(const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "digraph ComputeGraph {\n";
    out << "  rankdir=TB;\n";  // 从上到下
    out << "  splines=true;\n";
    out << "  nodesep=0.3;\n";
    out << "  ranksep=0.5;\n";

    // 构建图标题，如果是模板函数添加标记
    std::string graphLabel = EscapeDotString(name);
    if (GetProperty("is_template") == "true") {
        graphLabel += " [TEMPLATE]";
    }
    graphLabel += "\\nNodes: " + std::to_string(nodes.size())
               + ", Edges: " + std::to_string(edges.size());

    out << "  graph [fontname=\"Helvetica\", fontsize=14, label=\""
        << graphLabel << "\", labelloc=t];\n";
    out << "  node [shape=record, fontname=\"Courier\", fontsize=9];\n";
    out << "  edge [fontname=\"Helvetica\", fontsize=8];\n\n";

    // 收集所有涉及的函数，为每个函数分配颜色
    std::map<const clang::FunctionDecl*, std::string> funcColors;
    std::vector<std::string> colorPalette = {
        "#cce5ff",  // 浅蓝
        "#d4edda",  // 浅绿
        "#fff3cd",  // 浅黄
        "#f8d7da",  // 浅红
        "#e2e3e5",  // 浅灰
        "#d1ecf1",  // 浅青
        "#ffeeba",  // 橙黄
        "#c3e6cb",  // 薄荷绿
    };
    int colorIdx = 0;
    for (const auto& [id, node] : nodes) {
        if (node->containingFunc && funcColors.find(node->containingFunc) == funcColors.end()) {
            funcColors[node->containingFunc] = colorPalette[colorIdx % colorPalette.size()];
            colorIdx++;
        }
    }

    // 输出图例（函数颜色说明）
    out << "  // Legend\n";
    out << "  subgraph cluster_legend {\n";
    out << "    label=\"Functions\";\n";
    out << "    style=dashed;\n";
    out << "    fontsize=10;\n";
    int legendIdx = 0;
    for (const auto& [func, color] : funcColors) {
        std::string funcName = func ? EscapeDotString(func->getNameAsString()) : "unknown";
        out << "    legend_" << legendIdx << " [label=\"" << funcName
            << "\", fillcolor=\"" << color << "\", style=filled];\n";
        legendIdx++;
    }
    out << "  }\n\n";

    // 输出所有节点（带完整属性）
    out << "  // Nodes\n";
    for (const auto& [id, node] : nodes) {
        WriteDetailedNode(out, node, funcColors);
    }

    out << "\n  // Edges\n";

    // 输出边（带类型标签）
    for (const auto& [id, edge] : edges) {
        out << "  n" << edge->sourceId << " -> n" << edge->targetId;
        out << " [" << GetDetailedEdgeStyle(edge) << "];\n";
    }

    out << "}\n";

    llvm::outs() << "ComputeGraph exported to: " << filename << "\n";
}

void ComputeGraph::WriteDetailedNode(
    llvm::raw_fd_ostream& out,
    const NodePtr& node,
    const std::map<const clang::FunctionDecl*, std::string>& funcColors) const
{
    out << "  n" << node->id << " [label=\"{";

    // 第1行：ID和类型
    out << "[" << node->id << "] " << ComputeNodeKindToString(node->kind);

    // 第2行：名称/值
    out << " | ";
    if (!node->name.empty()) {
        out << "name: " << EscapeDotString(node->name);
    }
    if (node->hasConstValue) {
        bool isInt = (node->dataType.baseType == DataTypeInfo::BaseType::Int8 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::Int16 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::Int32 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::Int64 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::UInt8 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::UInt16 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::UInt32 ||
                      node->dataType.baseType == DataTypeInfo::BaseType::UInt64);
        if (isInt) {
            out << " val=" << node->constValue.intValue;
        } else {
            out << " val=" << node->constValue.floatValue;
        }
    }

    // 第3行：操作码
    if (node->opCode != OpCode::Unknown) {
        out << " | op: " << EscapeDotString(OpCodeToString(node->opCode));
    }

    // 第4行：数据类型
    out << " | type: " << EscapeDotString(node->dataType.ToString());

    // 第5行：所属函数
    out << " | func: ";
    if (node->containingFunc) {
        out << EscapeDotString(node->containingFunc->getNameAsString());
    } else {
        out << "?";
    }

    // 第6行：源码位置
    if (node->sourceLine > 0) {
        out << " | line: " << node->sourceLine;
    }

    // 第7行：源码片段
    if (!node->sourceText.empty()) {
        std::string text = node->sourceText;
        if (text.length() > 30) {
            text = text.substr(0, 27) + "...";
        }
        out << " | code: " << EscapeDotString(text);
    }

    // 第8行：关键属性
    std::string props;
    if (node->GetProperty("is_anchor") == "true") props += "ANCHOR ";
    if (node->GetProperty("is_loop_carried") == "true") props += "LOOP ";
    if (node->GetProperty("callee_analyzed") == "true") props += "EXPANDED ";
    if (node->GetProperty("is_formal_param") == "true") props += "FORMAL ";
    if (!props.empty()) {
        out << " | [" << props << "]";
    }

    // 第9行：调用点信息
    std::string callSiteId = node->GetProperty("call_site_id");
    if (!callSiteId.empty()) {
        out << " | ▶ CALL_SITE[" << callSiteId << "]";
        std::string calleeName = node->GetProperty("callee_name");
        if (!calleeName.empty()) {
            out << " from " << EscapeDotString(calleeName);
        }
    }

    // 【核心修复】第10行：循环上下文信息
    // 优先检查 loop_context 属性（防止字段复制丢失），然后检查 ID
    std::string loopContextStr = node->GetProperty("loop_context");
    if (!loopContextStr.empty()) {
        out << " | ★ " << EscapeDotString(loopContextStr);
        // 如果属性存在，ID 可能是0也无所谓，但如果字段还在，补充更多信息
        if (!node->loopContextVar.empty()) {
            out << " var=" << EscapeDotString(node->loopContextVar);
        }
        if (node->loopContextLine > 0) {
            out << " @L" << node->loopContextLine;
        }
    }
    else if (node->loopContextId != 0) {
        out << " | ★ IN LOOP[" << node->loopContextId << "]";
        if (!node->loopContextVar.empty()) {
            out << " var=" << EscapeDotString(node->loopContextVar);
        }
        if (node->loopContextLine > 0) {
            out << " @L" << node->loopContextLine;
        }
    }

    // 【新增】第11行：分支上下文 (branch_label)
    std::string branchLabel = node->GetProperty("branch_label");
    if (!branchLabel.empty()) {
        out << " | ◆ BRANCH: " << EscapeDotString(branchLabel);
    } else if (node->branchContextId != 0) {
        out << " | ◆ BRANCH[" << node->branchContextId << "]";
    }

    out << "}\"";

    // 颜色设置
    std::string fillColor = "#f0f0f0";
    if (node->containingFunc) {
        auto it = funcColors.find(node->containingFunc);
        if (it != funcColors.end()) {
            fillColor = it->second;
        }
    }
    out << ", style=filled, fillcolor=\"" << fillColor << "\"";

    if (node->GetProperty("is_anchor") == "true") {
        out << ", penwidth=3, color=red";
    } else if (node->GetProperty("callee_analyzed") == "true") {
        out << ", penwidth=2, color=blue";
    }

    out << "];\n";
}

std::string ComputeGraph::GetDetailedEdgeStyle(const EdgePtr& edge) const
{
    std::ostringstream oss;

    // 边类型标签
    std::string typeLabel = ComputeEdgeKindToString(edge->kind);
    if (!edge->label.empty()) {
        typeLabel += ": " + edge->label;
    }
    oss << "label=\"" << EscapeDotString(typeLabel) << "\"";

    // 边样式
    switch (edge->kind) {
        case ComputeEdgeKind::DataFlow:
            oss << ", color=\"#0066cc\", penwidth=1.5";
            break;
        case ComputeEdgeKind::Control:
            // 检查是否是CFG边
            if (edge->label.find("cfg") == 0) {
                // CFG边：绿色虚线
                oss << ", color=\"#00cc00\", style=dashed, penwidth=1.0";
            } else {
                // 其他控制依赖：红色点线
                oss << ", color=\"#cc0000\", style=dotted, penwidth=1.0";
            }
            break;
        case ComputeEdgeKind::LoopCarried:
            oss << ", color=\"#cc0000\", penwidth=2, style=bold";
            break;
        case ComputeEdgeKind::Return:
            oss << ", color=\"#ff6600\", penwidth=2, style=bold";
            break;
        case ComputeEdgeKind::Call:
            oss << ", color=\"#006600\", penwidth=2";
            break;
        case ComputeEdgeKind::Memory:
            oss << ", color=\"#660066\", style=dotted, penwidth=1.5";
            break;
    }

    return oss.str();
}

void ComputeGraph::ExportDotFileEnhanced(const std::string& filename) const
{
    std::error_code EC;
    llvm::raw_fd_ostream out(filename, EC);
    if (EC) {
        llvm::errs() << "Cannot create file: " << filename << "\n";
        return;
    }

    out << "digraph ComputeGraph {\n";
    out << "  rankdir=TB;\n";
    out << "  compound=true;\n";
    out << "  graph [fontname=\"Helvetica\", fontsize=14, ";
    out << "label=\"ComputeGraph: " << EscapeDotString(name);

    // 【新增】显示模板标记
    if (GetProperty("is_template") == "true") {
        out << " [TEMPLATE]";
    }
    out << "\\n";

    out << "Nodes: " << nodes.size() << ", Edges: " << edges.size() << "\\n";
    if (!properties.empty()) {
        out << "Loop Depth: " << GetProperty("loop_depth");
    }
    out << "\", labelloc=t, style=filled, fillcolor=white];\n";
    out << "  node [shape=record, fontname=\"Courier\", fontsize=9];\n";
    out << "  edge [fontname=\"Helvetica\", fontsize=8];\n\n";

    // 按类型分组节点
    std::map<ComputeNodeKind, std::vector<ComputeNode::NodeId>> nodesByKind;
    for (const auto& [id, node] : nodes) {
        nodesByKind[node->kind].push_back(id);
    }

    // 输入节点子图（Parameter, Constant）
    auto inputKinds = {ComputeNodeKind::Parameter, ComputeNodeKind::Constant};
    bool hasInputs = false;
    for (auto kind : inputKinds) {
        if (nodesByKind.count(kind)) hasInputs = true;
    }
    if (hasInputs) {
        out << "  subgraph cluster_inputs {\n";
        out << "    label=\"Inputs\";\n";
        out << "    style=rounded;\n";
        out << "    color=gray;\n";
        for (auto kind : inputKinds) {
            for (auto id : nodesByKind[kind]) {
                WriteNodeDotEnhanced(out, nodes.at(id));
            }
        }
        out << "  }\n\n";
    }

    // 计算节点子图（BinaryOp, UnaryOp, Call）
    auto computeKinds = {ComputeNodeKind::BinaryOp, ComputeNodeKind::UnaryOp,
                         ComputeNodeKind::Call, ComputeNodeKind::Cast};
    bool hasCompute = false;
    for (auto kind : computeKinds) {
        if (nodesByKind.count(kind)) hasCompute = true;
    }
    if (hasCompute) {
        out << "  subgraph cluster_compute {\n";
        out << "    label=\"Computation\";\n";
        out << "    style=rounded;\n";
        out << "    color=green;\n";
        for (auto kind : computeKinds) {
            for (auto id : nodesByKind[kind]) {
                WriteNodeDotEnhanced(out, nodes.at(id));
            }
        }
        out << "  }\n\n";
    }

    // 内存操作子图（Load, Store, ArrayAccess）
    auto memKinds = {ComputeNodeKind::Load, ComputeNodeKind::Store,
                     ComputeNodeKind::ArrayAccess};
    bool hasMem = false;
    for (auto kind : memKinds) {
        if (nodesByKind.count(kind)) hasMem = true;
    }
    if (hasMem) {
        out << "  subgraph cluster_memory {\n";
        out << "    label=\"Memory\";\n";
        out << "    style=rounded;\n";
        out << "    color=purple;\n";
        for (auto kind : memKinds) {
            for (auto id : nodesByKind[kind]) {
                WriteNodeDotEnhanced(out, nodes.at(id));
            }
        }
        out << "  }\n\n";
    }

    // 其他节点
    out << "  // Other nodes\n";
    std::set<ComputeNodeKind> handledKinds = {
        ComputeNodeKind::Parameter, ComputeNodeKind::Constant,
        ComputeNodeKind::BinaryOp, ComputeNodeKind::UnaryOp,
        ComputeNodeKind::Call, ComputeNodeKind::Cast,
        ComputeNodeKind::Load, ComputeNodeKind::Store, ComputeNodeKind::ArrayAccess
    };
    for (const auto& [id, node] : nodes) {
        if (handledKinds.find(node->kind) == handledKinds.end()) {
            WriteNodeDotEnhanced(out, node);
        }
    }

    // 输出边
    out << "\n  // Edges\n";
    for (const auto& [id, edge] : edges) {
        out << "  n" << edge->sourceId << " -> n" << edge->targetId;
        out << " [" << GetEdgeDotStyleEnhanced(edge) << "];\n";
    }

    out << "}\n";

    llvm::outs() << "Enhanced ComputeGraph exported to: " << filename << "\n";
}

void ComputeGraph::WriteNodeDotEnhanced(llvm::raw_fd_ostream& out,
                                         const NodePtr& node) const
{
    out << "    n" << node->id << " [";
    out << "label=\"{";

    // 第一行：节点类型和名称
    out << "[" << node->id << "] " << ComputeNodeKindToString(node->kind);
    if (!node->name.empty()) {
        out << ": " << EscapeDotString(node->name);
    }

    // 第二行：操作码（如果有）- 可能包含 < > | 等特殊字符
    if (node->opCode != OpCode::Unknown) {
        out << " | op: " << EscapeDotString(OpCodeToString(node->opCode));
    }

    // 第三行：数据类型 - 可能包含模板参数
    out << " | type: " << EscapeDotString(node->dataType.ToString());

    // 第四行：所属函数
    if (node->containingFunc) {
        out << " | func: " << EscapeDotString(node->containingFunc->getNameAsString());
    }

    // 第五行：源码位置
    if (node->sourceLine > 0) {
        out << " | line: " << node->sourceLine;
    }

    // 【新增】第六行：调用点信息
    std::string callSiteId = node->GetProperty("call_site_id");
    if (!callSiteId.empty()) {
        out << " | ▶ CALL[" << callSiteId << "]";
    }

    // 第七行：循环上下文信息
    if (node->loopContextId != 0) {
        out << " | ★ LOOP[" << node->loopContextId << "]";
        if (!node->loopContextVar.empty()) {
            out << " var=" << EscapeDotString(node->loopContextVar);
        }
        if (node->loopContextLine > 0) {
            out << " @L" << node->loopContextLine;
        }
    }

    // 【新增】第八行：分支上下文信息
    if (node->branchContextId != 0) {
        out << " | ◆ BRANCH[" << node->branchContextId << "]";
        if (!node->branchType.empty()) {
            out << " " << node->branchType;
        }
        if (node->branchContextLine > 0) {
            out << " @L" << node->branchContextLine;
        }
    }

    out << "}\"";
    out << ", style=filled, fillcolor=" << GetNodeDotColor(node);

    // 锚点节点特殊标记
    if (node->GetProperty("is_anchor") == "true") {
        out << ", penwidth=3, color=red";
    }

    // 展开的函数调用标记
    if (node->GetProperty("callee_analyzed") == "true") {
        out << ", penwidth=2, color=blue";
    }

    // 【新增】在循环中的展开函数节点特殊标记
    if (node->loopContextId != 0 && !callSiteId.empty()) {
        out << ", peripheries=2";  // 双边框表示在循环中展开的函数节点
    }

    out << "];\n";
}

std::string ComputeGraph::GetEdgeDotStyleEnhanced(const EdgePtr& edge) const
{
    std::ostringstream oss;

    switch (edge->kind) {
        case ComputeEdgeKind::DataFlow:
            oss << "color=blue, penwidth=1.5";
            break;
        case ComputeEdgeKind::Control:
            oss << "color=red, style=dashed, penwidth=1.5";
            break;
        case ComputeEdgeKind::Memory:
            oss << "color=purple, style=dotted, penwidth=1.5";
            break;
        case ComputeEdgeKind::Call:
            oss << "color=\"#008800\", style=bold, penwidth=2";
            break;
        case ComputeEdgeKind::Return:
            oss << "color=orange, style=bold, penwidth=2, arrowhead=diamond";
            break;
        case ComputeEdgeKind::LoopCarried:
            oss << "color=brown, style=dashed, penwidth=2, constraint=false";
            break;
    }

    if (!edge->label.empty()) {
        oss << ", label=\"" << EscapeDotString(edge->label) << "\"";
    }

    return oss.str();
}

std::string ComputeGraph::GetNodeDotLabelEnhanced(const NodePtr& node) const
{
    std::ostringstream oss;

    // 节点类型简写
    oss << "[" << node->id << "] ";
    oss << ComputeNodeKindToString(node->kind);

    // 名称 - 需要转义特殊字符如 < >
    if (!node->name.empty()) {
        oss << ": " << EscapeDotString(node->name);
    }

    // 操作码 - 可能包含 < > | 等特殊字符
    if (node->opCode != OpCode::Unknown) {
        oss << " [" << EscapeDotString(OpCodeToString(node->opCode)) << "]";
    }

    // 数据类型 - 可能包含模板参数如 vector<int>
    oss << "\\n" << EscapeDotString(node->dataType.ToString());

    // 源码行号
    if (node->sourceLine > 0) {
        oss << " L" << node->sourceLine;
    }

    // 源码片段（截断）- 可能包含特殊字符
    if (!node->sourceText.empty()) {
        std::string text = node->sourceText;
        if (text.length() > 40) {
            text = text.substr(0, 37) + "...";
        }
        oss << "\\n" << EscapeDotString(text);
    }

    return oss.str();
}

std::string ComputeGraph::EscapeDotString(const std::string& str) const
{
    std::string result;
    for (char c : str) {
        switch (c) {
            case '"': result += "'"; break;      // 双引号换成单引号
            case '\\': result += "/"; break;     // 反斜杠换成正斜杠
            case '\n': result += " "; break;     // 换行换成空格
            case '\r': break;
            case '<': result += "\\<"; break;    // 【修复】< 需要转义为 \<
            case '>': result += "\\>"; break;    // 【修复】> 需要转义为 \>
            case '{': result += "\\{"; break;    // { 需要转义为 \{
            case '}': result += "\\}"; break;    // } 需要转义为 \}
            case '|': result += "\\|"; break;    // | 需要转义为 \|
            default: result += c; break;
        }
    }
    return result;
}

void ComputeGraph::Dump() const
{
    llvm::outs() << "\n========== ComputeGraph: " << name << " ==========\n";
    llvm::outs() << "Nodes: " << nodes.size() << ", Edges: " << edges.size() << "\n\n";

    llvm::outs() << "--- Nodes ---\n";
    for (const auto& [id, node] : nodes) {
        node->Dump();
    }

    llvm::outs() << "\n--- Edges ---\n";
    for (const auto& [id, edge] : edges) {
        llvm::outs() << "[Edge " << id << "] n" << edge->sourceId;
        llvm::outs() << " -> n" << edge->targetId;
        llvm::outs() << " (" << edge->GetLabel() << ")\n";
    }

    llvm::outs() << "================================================\n\n";
}

void ComputeGraph::PrintSummary() const
{
    llvm::outs() << "Graph '" << name << "': ";
    llvm::outs() << nodes.size() << " nodes, " << edges.size() << " edges\n";

    // 统计各类型节点数量
    std::map<ComputeNodeKind, int> kindCount;
    for (const auto& [id, node] : nodes) {
        kindCount[node->kind]++;
    }

    llvm::outs() << "  Node types: ";
    for (const auto& [kind, count] : kindCount) {
        llvm::outs() << ComputeNodeKindToString(kind) << "=" << count << " ";
    }
    llvm::outs() << "\n";
}

void ComputeGraph::SetProperty(const std::string& key, const std::string& value)
{
    properties[key] = value;
}

std::string ComputeGraph::GetProperty(const std::string& key) const
{
    auto it = properties.find(key);
    return it != properties.end() ? it->second : "";
}

bool ComputeGraph::HasProperty(const std::string& key) const
{
    return properties.find(key) != properties.end();
}

void ComputeGraph::UpdateAdjacencyLists(EdgePtr edge)
{
    outEdges[edge->sourceId].push_back(edge->id);
    inEdges[edge->targetId].push_back(edge->id);
}

std::string ComputeGraph::GetNodeDotLabel(const NodePtr& node) const
{
    return node->GetLabel();
}

std::string ComputeGraph::GetNodeDotColor(const NodePtr& node) const
{
    switch (node->kind) {
        case ComputeNodeKind::Constant: return "lightgray";
        case ComputeNodeKind::Variable: return "lightblue";
        case ComputeNodeKind::Parameter: return "lightyellow";
        case ComputeNodeKind::BinaryOp: return "lightgreen";
        case ComputeNodeKind::UnaryOp: return "lightgreen";
        case ComputeNodeKind::CompareOp: return "orange";
        case ComputeNodeKind::Load: return "pink";
        case ComputeNodeKind::Store: return "pink";
        case ComputeNodeKind::ArrayAccess: return "pink";
        case ComputeNodeKind::MemberAccess: return "pink";
        case ComputeNodeKind::Phi: return "cyan";
        case ComputeNodeKind::Select: return "cyan";
        case ComputeNodeKind::LoopInduction: return "cyan";
        case ComputeNodeKind::Loop: return "coral";        // 【新增】循环节点 - 珊瑚色
        case ComputeNodeKind::Branch: return "orchid";     // 【新增】分支节点 - 兰花紫
        case ComputeNodeKind::Call: return "yellow";
        case ComputeNodeKind::IntrinsicCall: return "gold";
        case ComputeNodeKind::Cast: return "lightgray";
        case ComputeNodeKind::Return: return "lightcoral";
        case ComputeNodeKind::Unknown: return "white";
    }
    return "white";  // 防止编译器警告
}

std::string ComputeGraph::GetEdgeDotStyle(const EdgePtr& edge) const
{
    switch (edge->kind) {
        case ComputeEdgeKind::DataFlow:
            return "color=blue";
        case ComputeEdgeKind::Control:
            return "color=red, style=dashed";
        case ComputeEdgeKind::Memory:
            return "color=purple, style=dotted";
        case ComputeEdgeKind::Call:
            return "color=green, style=bold";
        case ComputeEdgeKind::Return:
            return "color=orange, style=bold";
        case ComputeEdgeKind::LoopCarried:
            return "color=brown, style=dashed";
    }
    return "color=black";
}

// ============================================
// ComputeGraphSet 实现
// ============================================

void ComputeGraphSet::AddGraph(GraphPtr graph)
{
    graphs.push_back(graph);
}

void ComputeGraphSet::RemoveGraph(const std::string& name)
{
    graphs.erase(
        std::remove_if(graphs.begin(), graphs.end(),
            [&name](const GraphPtr& g) { return g->GetName() == name; }),
        graphs.end());
}

ComputeGraphSet::GraphPtr ComputeGraphSet::GetGraph(const std::string& name) const
{
    for (const auto& g : graphs) {
        if (g->GetName() == name) {
            return g;
        }
    }
    return nullptr;
}

std::vector<ComputeGraphSet::GraphPtr> ComputeGraphSet::GetAllGraphs() const
{
    return graphs;
}

void ComputeGraphSet::Deduplicate()
{
    std::vector<GraphPtr> unique;
    std::set<std::string> seenAnchors;  // 基于锚点位置去重
    std::set<std::string> seenSignatures;  // 基于结构去重

    for (const auto& g : graphs) {
        // 首先基于锚点位置去重（函数名+行号）
        std::string anchorKey = g->GetProperty("anchor_func") + ":" +
                                g->GetProperty("anchor_line");

        if (seenAnchors.find(anchorKey) != seenAnchors.end()) {
            // 已经有这个锚点的图了，跳过
            continue;
        }

        // 然后基于结构签名去重
        auto sig = g->ComputeCanonicalSignature();
        if (seenSignatures.find(sig) != seenSignatures.end()) {
            // 结构相同的图也跳过
            continue;
        }

        seenAnchors.insert(anchorKey);
        seenSignatures.insert(sig);
        unique.push_back(g);
    }

    graphs = std::move(unique);
}

void ComputeGraphSet::MergeOverlapping()
{
    auto& graphsRef = graphs;

    bool changed = true;
    while (changed) {
        changed = false;

        for (size_t i = 0; i < graphsRef.size() && !changed; ++i) {
            for (size_t j = i + 1; j < graphsRef.size() && !changed; ++j) {
                if (ComputeGraphMerger::HasOverlap(*graphsRef[i], *graphsRef[j])) {
                    // 合并两个图
                    auto merged = ComputeGraphMerger::Merge(*graphsRef[i], *graphsRef[j]);
                    graphsRef[i] = merged;
                    graphsRef.erase(graphsRef.begin() + j);
                    changed = true;
                }
            }
        }
    }
}

void ComputeGraphSet::SortByScore()
{
    std::sort(graphs.begin(), graphs.end(),
        [](const GraphPtr& a, const GraphPtr& b) {
            int scoreA = std::stoi(a->GetProperty("score"));
            int scoreB = std::stoi(b->GetProperty("score"));
            return scoreA > scoreB;
        });
}

void ComputeGraphSet::Dump() const
{
    llvm::outs() << "\n========== ComputeGraphSet ==========\n";
    llvm::outs() << "Total graphs: " << graphs.size() << "\n\n";

    int idx = 0;
    for (const auto& g : graphs) {
        llvm::outs() << "[" << idx++ << "] ";
        g->PrintSummary();
    }

    llvm::outs() << "=====================================\n\n";
}

void ComputeGraphSet::ExportAllDotFiles(const std::string& outputDir) const
{
    llvm::outs() << "\nExporting " << graphs.size() << " graphs to " << outputDir << "\n";

    // 创建输出目录
    std::error_code EC = llvm::sys::fs::create_directories(outputDir);
    if (EC) {
        llvm::errs() << "Cannot create directory: " << outputDir << "\n";
        return;
    }

    int idx = 0;
    for (const auto& g : graphs) {
        std::string filename = outputDir + "/cg_" + std::to_string(idx) + "_" +
                               g->GetName() + ".dot";
        g->ExportDotFile(filename);
        idx++;
    }

    llvm::outs() << "Exported " << idx << " DOT files\n";
}

void ComputeGraphSet::ExportAllDotFilesEnhanced(const std::string& outputDir) const
{
    llvm::outs() << "\nExporting " << graphs.size() << " enhanced graphs to " << outputDir << "\n";

    // 创建输出目录
    std::error_code EC = llvm::sys::fs::create_directories(outputDir);
    if (EC) {
        llvm::errs() << "Cannot create directory: " << outputDir << "\n";
        return;
    }

    int idx = 0;
    for (const auto& g : graphs) {
        std::string filename = outputDir + "/cg_enhanced_" + std::to_string(idx) + "_" +
                               g->GetName() + ".dot";
        g->ExportDotFileEnhanced(filename);
        idx++;
    }

    llvm::outs() << "Exported " << idx << " enhanced DOT files\n";
}

// ============================================
// 辅助函数实现
// ============================================

std::string OpCodeToString(OpCode op)
{
    switch (op) {
        case OpCode::Add: return "+";
        case OpCode::Sub: return "-";
        case OpCode::Mul: return "*";
        case OpCode::Div: return "/";
        case OpCode::Mod: return "%";
        case OpCode::And: return "&";
        case OpCode::Or: return "|";
        case OpCode::Xor: return "^";
        case OpCode::Shl: return "<<";
        case OpCode::Shr: return ">>";
        case OpCode::Neg: return "neg";
        case OpCode::Not: return "!";
        case OpCode::BitNot: return "~";
        case OpCode::Lt: return "<";
        case OpCode::Gt: return ">";
        case OpCode::Le: return "<=";
        case OpCode::Ge: return ">=";
        case OpCode::Eq: return "==";
        case OpCode::Ne: return "!=";
        case OpCode::Assign: return "=";
        default: return "?";
    }
}

OpCode StringToOpCode(const std::string& str)
{
    if (str == "+" || str == "Add") return OpCode::Add;
    if (str == "-" || str == "Sub") return OpCode::Sub;
    if (str == "*" || str == "Mul") return OpCode::Mul;
    if (str == "/" || str == "Div") return OpCode::Div;
    if (str == "%" || str == "Mod") return OpCode::Mod;
    if (str == "&" || str == "And") return OpCode::And;
    if (str == "|" || str == "Or") return OpCode::Or;
    if (str == "^" || str == "Xor") return OpCode::Xor;
    if (str == "<<" || str == "Shl") return OpCode::Shl;
    if (str == ">>" || str == "Shr") return OpCode::Shr;
    if (str == "<" || str == "Lt") return OpCode::Lt;
    if (str == ">" || str == "Gt") return OpCode::Gt;
    if (str == "<=" || str == "Le") return OpCode::Le;
    if (str == ">=" || str == "Ge") return OpCode::Ge;
    if (str == "==" || str == "Eq") return OpCode::Eq;
    if (str == "!=" || str == "Ne") return OpCode::Ne;
    if (str == "=" || str == "Assign") return OpCode::Assign;
    return OpCode::Unknown;
}

std::string ComputeNodeKindToString(ComputeNodeKind kind)
{
    switch (kind) {
        case ComputeNodeKind::Constant: return "Const";
        case ComputeNodeKind::Variable: return "Var";
        case ComputeNodeKind::Parameter: return "Param";
        case ComputeNodeKind::BinaryOp: return "BinOp";
        case ComputeNodeKind::UnaryOp: return "UnaryOp";
        case ComputeNodeKind::CompareOp: return "CmpOp";
        case ComputeNodeKind::Load: return "Load";
        case ComputeNodeKind::Store: return "Store";
        case ComputeNodeKind::ArrayAccess: return "ArrayAccess";
        case ComputeNodeKind::MemberAccess: return "MemberAccess";
        case ComputeNodeKind::Phi: return "Phi";
        case ComputeNodeKind::Select: return "Select";
        case ComputeNodeKind::LoopInduction: return "LoopInd";
        case ComputeNodeKind::Loop: return "Loop";           // 【新增】
        case ComputeNodeKind::Branch: return "Branch";       // 【新增】
        case ComputeNodeKind::Call: return "Call";
        case ComputeNodeKind::IntrinsicCall: return "Intrinsic";
        case ComputeNodeKind::Cast: return "Cast";
        case ComputeNodeKind::Return: return "Return";
        default: return "Unknown";
    }
}

std::string ComputeEdgeKindToString(ComputeEdgeKind kind)
{
    switch (kind) {
        case ComputeEdgeKind::DataFlow: return "DataFlow";
        case ComputeEdgeKind::Control: return "Control";
        case ComputeEdgeKind::Memory: return "Memory";
        case ComputeEdgeKind::Call: return "Call";
        case ComputeEdgeKind::Return: return "Return";
        case ComputeEdgeKind::LoopCarried: return "LoopCarried";
    }
    return "Unknown";
}

// ============================================
// PatternMatcher 实现
// ============================================

void PatternMatcher::RegisterPattern(const RewritePattern& pattern)
{
    patterns[pattern.name] = pattern;
}

std::vector<std::string> PatternMatcher::GetRegisteredPatterns() const
{
    std::vector<std::string> result;
    for (const auto& [name, _] : patterns) {
        result.push_back(name);
    }
    return result;
}

std::vector<std::map<int, ComputeNode::NodeId>> PatternMatcher::FindMatches(
    const ComputeGraph& graph, const std::string& patternName)
{
    std::vector<std::map<int, ComputeNode::NodeId>> allMatches;

    auto it = patterns.find(patternName);
    if (it == patterns.end()) {
        return allMatches;
    }

    const auto& rewritePattern = it->second;
    if (rewritePattern.pattern.empty()) {
        return allMatches;
    }

    // 从第一个模式节点开始尝试匹配
    const auto& firstPatternNode = rewritePattern.pattern[0];

    for (const auto& node : graph.GetAllNodes()) {
        std::map<int, ComputeNode::NodeId> bindings;

        if (MatchNode(graph, node->id, firstPatternNode, bindings)) {
            // 尝试匹配剩余的模式节点
            bool fullMatch = true;

            for (size_t i = 1; i < rewritePattern.pattern.size() && fullMatch; ++i) {
                const auto& patternNode = rewritePattern.pattern[i];
                bool nodeMatched = false;

                // 检查是否已经绑定
                if (bindings.count(patternNode.captureId)) {
                    auto boundNodeId = bindings[patternNode.captureId];
                    auto boundNode = graph.GetNode(boundNodeId);
                    if (boundNode && boundNode->kind == patternNode.kind) {
                        nodeMatched = true;
                    }
                } else {
                    // 尝试找一个匹配的节点
                    for (const auto& candidateNode : graph.GetAllNodes()) {
                        if (MatchNode(graph, candidateNode->id, patternNode, bindings)) {
                            nodeMatched = true;
                            break;
                        }
                    }
                }

                if (!nodeMatched) {
                    fullMatch = false;
                }
            }

            if (fullMatch) {
                allMatches.push_back(bindings);
            }
        }
    }

    return allMatches;
}

bool PatternMatcher::MatchNode(
    const ComputeGraph& graph,
    ComputeNode::NodeId nodeId,
    const PatternNode& patternNode,
    std::map<int, ComputeNode::NodeId>& bindings)
{
    auto node = graph.GetNode(nodeId);
    if (!node) return false;

    // 检查节点类型
    if (patternNode.kind != ComputeNodeKind::Unknown &&
        node->kind != patternNode.kind) {
        return false;
    }

    // 检查操作码（如果指定）
    if (patternNode.opCode != OpCode::Unknown &&
        node->opCode != patternNode.opCode) {
        return false;
    }

    // 如果有captureId，检查是否已经绑定到不同的节点
    if (patternNode.captureId >= 0) {
        auto it = bindings.find(patternNode.captureId);
        if (it != bindings.end()) {
            return it->second == nodeId;
        }

        // 添加绑定
        bindings[patternNode.captureId] = nodeId;
    }

    return true;
}

std::shared_ptr<ComputeGraph> PatternMatcher::ApplyRewrite(
    const ComputeGraph& graph,
    const std::string& patternName,
    const std::map<int, ComputeNode::NodeId>& match)
{
    auto it = patterns.find(patternName);
    if (it == patterns.end()) {
        return nullptr;
    }

    // 简化实现：创建一个图的副本
    auto result = std::make_shared<ComputeGraph>(graph.GetName() + "_rewritten");

    // 复制所有节点
    std::map<ComputeNode::NodeId, ComputeNode::NodeId> nodeMap;
    for (const auto& node : graph.GetAllNodes()) {
        auto newNode = result->CreateNode(node->kind);
        newNode->name = node->name;
        newNode->opCode = node->opCode;
        newNode->dataType = node->dataType;
        nodeMap[node->id] = newNode->id;
    }

    // 复制所有边
    for (const auto& edge : graph.GetAllEdges()) {
        result->AddEdge(nodeMap[edge->sourceId], nodeMap[edge->targetId],
                        edge->kind, edge->label);
    }

    // TODO: 实际的重写逻辑需要根据RewritePattern的定义来实现

    return result;
}

} // namespace compute_graph