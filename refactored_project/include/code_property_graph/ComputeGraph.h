/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * ComputeGraph.h - 计算图抽象层，用于向量化模式匹配和改写
 */
#ifndef COMPUTE_GRAPH_H
#define COMPUTE_GRAPH_H

#include "clang/AST/ASTContext.h"
#include "ComputeGraphAnchor.h"
#include "ComputeGraph.h"
#include "ComputeGraphBase.h"
#include "code_property_graph/CPGAnnotation.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// namespace cpg {
//     class CPGContext;
// }

namespace compute_graph {

// ============================================
// 计算图节点
// ============================================
class ComputeNode {
public:
    using NodeId = uint64_t;

    ComputeNodeKind kind;
    NodeId id;
    std::string name;               // 节点名称/标签
    DataTypeInfo dataType;          // 数据类型

    // AST关联
    const clang::Stmt* astStmt = nullptr;
    const clang::Decl* astDecl = nullptr;
    const clang::FunctionDecl* containingFunc = nullptr;

    // 运算相关
    OpCode opCode = OpCode::Unknown;

    // 常量值 (如果是Constant节点)
    union {
        int64_t intValue;
        double floatValue;
    } constValue;
    bool hasConstValue = false;

    // 属性映射
    std::map<std::string, std::string> properties;

    // 连接信息 (由ComputeGraph管理)
    std::vector<NodeId> inputNodes;     // 输入节点ID列表
    std::vector<NodeId> outputNodes;    // 输出节点ID列表

    // 循环相关信息
    int loopDepth = 0;                  // 循环嵌套深度
    bool isLoopInvariant = false;       // 是否循环不变量

    // 【新增】循环上下文信息（用于跨函数展开时保留循环信息）
    NodeId loopContextId = 0;           // 所在循环节点的ID (0表示不在循环中)
    std::string loopContextVar;         // 循环变量名
    int loopContextLine = 0;            // 循环所在行号（用于代码生成定位）

    // 【新增】分支上下文信息
    NodeId branchContextId = 0;         // 所在分支节点的ID (0表示不在分支中)
    std::string branchType;             // 分支类型："THEN", "ELSE", 或空
    int branchContextLine = 0;          // 分支所在行号

    // 源码信息
    std::string sourceText;             // 对应的源码文本
    int sourceLine = 0;                 // 源码行号

    explicit ComputeNode(ComputeNodeKind k, NodeId nodeId);

    std::string GetLabel() const;
    std::string GetKindName() const;
    void SetProperty(const std::string& key, const std::string& value);
    std::string GetProperty(const std::string& key) const;
    bool HasProperty(const std::string& key) const;

    // 判断节点是否可向量化
    bool IsVectorizable() const;
    // 判断是否是运算节点
    bool IsOperationNode() const;
    // 判断是否是内存节点
    bool IsMemoryNode() const;

    void Dump() const;
};

// ============================================
// 计算图边
// ============================================
class ComputeEdge {
public:
    using EdgeId = uint64_t;

    EdgeId id;
    ComputeEdgeKind kind;
    ComputeNode::NodeId sourceId;
    ComputeNode::NodeId targetId;

    // 边的标签 (变量名或描述)
    std::string label;

    // 边的权重/优先级
    int weight = 1;

    // 属性
    std::map<std::string, std::string> properties;

    ComputeEdge(EdgeId edgeId, ComputeEdgeKind k,
                ComputeNode::NodeId src, ComputeNode::NodeId tgt);

    std::string GetLabel() const;
    std::string GetKindName() const;
};



// ============================================
// 计算图
// ============================================
class ComputeGraph {
public:
    using NodePtr = std::shared_ptr<ComputeNode>;
    using EdgePtr = std::shared_ptr<ComputeEdge>;

    explicit ComputeGraph(const std::string& graphName = "");

    // ========================================
    // 节点操作
    // ========================================
    NodePtr CreateNode(ComputeNodeKind kind);
    NodePtr GetNode(ComputeNode::NodeId id) const;
    NodePtr FindNodeByStmt(const clang::Stmt* stmt) const;
    NodePtr FindNodeByName(const std::string& name) const;
    void RemoveNode(ComputeNode::NodeId id);

    // ========================================
    // 边操作
    // ========================================
    EdgePtr AddEdge(ComputeNode::NodeId src, ComputeNode::NodeId tgt,
                    ComputeEdgeKind kind, const std::string& varName = "");
    EdgePtr GetEdge(ComputeEdge::EdgeId id) const;
    void RemoveEdge(ComputeEdge::EdgeId id);

    // 获取节点的输入/输出边
    std::vector<EdgePtr> GetIncomingEdges(ComputeNode::NodeId nodeId) const;
    std::vector<EdgePtr> GetOutgoingEdges(ComputeNode::NodeId nodeId) const;

    // ========================================
    // 图遍历
    // ========================================
    std::vector<NodePtr> GetAllNodes() const;
    std::vector<EdgePtr> GetAllEdges() const;
    std::vector<NodePtr> GetRootNodes() const;      // 无输入的节点
    std::vector<NodePtr> GetLeafNodes() const;      // 无输出的节点

    // 拓扑排序
    std::vector<NodePtr> TopologicalSort() const;

    // ========================================
    // 图属性
    // ========================================
    std::string GetName() const { return name; }
    void SetName(const std::string& n) { name = n; }
    size_t NodeCount() const { return nodes.size(); }
    size_t EdgeCount() const { return edges.size(); }
    bool IsEmpty() const { return nodes.empty(); }

    // ========================================
    // 图操作
    // ========================================
    // 合并另一个图
    void Merge(const ComputeGraph& other);
    // 创建子图
    ComputeGraph ExtractSubgraph(const std::set<ComputeNode::NodeId>& nodeIds) const;
    // 克隆图
    ComputeGraph Clone() const;
    // 清空图
    void Clear();

    // 获取所有节点 (用于遍历)
    const std::map<ComputeNode::NodeId, NodePtr>& GetNodes() const { return nodes; }

    // 获取所有边 (用于遍历)
    const std::map<ComputeEdge::EdgeId, EdgePtr>& GetEdges() const { return edges; }

    // ========================================
    // 图规范化
    // ========================================
    // 计算图的规范化签名 (用于去重)
    std::string ComputeCanonicalSignature() const;
    // 判断两个图是否同构
    bool IsIsomorphicTo(const ComputeGraph& other) const;

    // ========================================
    // 可视化
    // ========================================
    void ExportDotFile(const std::string& filename) const;
    void ExportDotFileEnhanced(const std::string& filename) const;
    void Dump() const;
    void PrintSummary() const;

    // ========================================
    // 属性
    // ========================================
    void SetProperty(const std::string& key, const std::string& value);
    std::string GetProperty(const std::string& key) const;
    bool HasProperty(const std::string& key) const;

private:
    std::string name;
    ComputeNode::NodeId nextNodeId = 1;  // 从1开始，0作为无效ID
    ComputeEdge::EdgeId nextEdgeId = 0;

    std::map<ComputeNode::NodeId, NodePtr> nodes;
    std::map<ComputeEdge::EdgeId, EdgePtr> edges;

    // 快速查找映射
    std::map<const clang::Stmt*, ComputeNode::NodeId> stmtToNode;
    std::map<std::string, ComputeNode::NodeId> nameToNode;

    // 邻接表
    std::map<ComputeNode::NodeId, std::vector<ComputeEdge::EdgeId>> inEdges;
    std::map<ComputeNode::NodeId, std::vector<ComputeEdge::EdgeId>> outEdges;

    // 图属性
    std::map<std::string, std::string> properties;

    // 辅助方法
    void UpdateAdjacencyLists(EdgePtr edge);
    std::string GetNodeDotLabel(const NodePtr& node) const;
    std::string GetNodeDotColor(const NodePtr& node) const;
    std::string GetEdgeDotStyle(const EdgePtr& edge) const;

    // 详细可视化辅助方法
    void WriteDetailedNode(llvm::raw_fd_ostream& out, const NodePtr& node,
                           const std::map<const clang::FunctionDecl*, std::string>& funcColors) const;
    std::string GetDetailedEdgeStyle(const EdgePtr& edge) const;

    // 增强可视化辅助方法
    void WriteNodeDotEnhanced(llvm::raw_fd_ostream& out, const NodePtr& node) const;
    std::string GetNodeDotLabelEnhanced(const NodePtr& node) const;
    std::string GetEdgeDotStyleEnhanced(const EdgePtr& edge) const;
    std::string EscapeDotString(const std::string& str) const;
};

// ============================================
// 计算图集合 (管理多个计算图)
// ============================================
class ComputeGraphSet {
public:
    using GraphPtr = std::shared_ptr<ComputeGraph>;

    void AddGraph(GraphPtr graph);
    void RemoveGraph(const std::string& name);
    GraphPtr GetGraph(const std::string& name) const;
    std::vector<GraphPtr> GetAllGraphs() const;

    // 获取可修改的图列表引用 (用于合并等操作)
    std::vector<GraphPtr>& GetGraphsRef() { return graphs; }

    // 去重：移除同构图
    void Deduplicate();

    // 合并可合并的图
    void MergeOverlapping();

    // 按评分排序
    void SortByScore();

    size_t Size() const { return graphs.size(); }
    void Clear() { graphs.clear(); }

    void Dump() const;

    // 批量导出所有图到DOT文件
    void ExportAllDotFiles(const std::string& outputDir) const;
    void ExportAllDotFilesEnhanced(const std::string& outputDir) const;

private:
    std::vector<GraphPtr> graphs;
};


// ============================================
// 计算图构建器
// ============================================
class ComputeGraphBuilder {
public:
    explicit ComputeGraphBuilder(cpg::CPGContext& cpgCtx, clang::ASTContext& astCtx);

    // 从锚点构建计算图
    std::shared_ptr<ComputeGraph> BuildFromAnchor(const AnchorPoint& anchor);

    // 从表达式构建计算图
    std::shared_ptr<ComputeGraph> BuildFromExpr(const clang::Expr* expr);

    // 从函数构建完整计算图
    std::shared_ptr<ComputeGraph> BuildFromFunction(const clang::FunctionDecl* func);

    ComputeNode::NodeId CreateDefinitionNode(
const clang::Stmt* defStmt,
const std::string& varName);

    // 配置
    void SetMaxBackwardDepth(int depth) { maxBackwardDepth = depth; }
    void SetMaxForwardDepth(int depth) { maxForwardDepth = depth; }
    void SetMaxCallDepth(int depth) { maxCallDepth = depth; }
    void SetMaxExprDepth(int depth) { maxExprDepth = depth; }
    void SetEnableInterprocedural(bool enable) { enableInterprocedural = enable; }

    ComputeNode::NodeId BuildSwitchBranch(
    const clang::SwitchStmt* switchStmt, int depth);

private:
    cpg::CPGContext& cpgContext;
    clang::ASTContext& astContext;

    int maxBackwardDepth = 10;      // 最大向后追踪深度
    int maxForwardDepth = 5;        // 最大向前追踪深度
    int maxCallDepth = 3;           // 最大函数调用深度
    int maxExprDepth = 20;          // 最大表达式递归深度
    bool enableInterprocedural = true;  // 是否启用跨函数分析

    void ProcessSwitchBody(
        const clang::CompoundStmt* body,
        ComputeNode::NodeId switchId,
        int depth);
    void ProcessSwitchCasesSimple(
        const clang::Stmt* body,
        ComputeNode::NodeId switchId,
        int depth);
    // 已处理的语句映射
    std::map<const clang::Stmt*, ComputeNode::NodeId> processedStmts;

    // 【新增】已进行前向追踪的语句集合（防止重复追踪）
    std::set<const clang::Stmt*> forwardTracedStmts;

    // 已处理的函数集合 (防止无限递归) - 不再使用
    std::set<const clang::FunctionDecl*> processedFunctions;

    // 【新增】当前调用栈 (用于检测递归调用)
    std::set<const clang::FunctionDecl*> currentCallStack;

    // 当前函数调用深度
    int currentCallDepth = 0;

    void AddCFGEdges();

    std::string GetCFGEdgeLabel(cpg::ICFGEdgeKind kind);

    // 当前正在构建的图
    std::shared_ptr<ComputeGraph> currentGraph;

    // 【新增】循环信息结构体（定义在使用之前）
    struct LoopInfo {
        ComputeNode::NodeId loopNodeId = 0;
        const clang::Stmt* loopStmt = nullptr;
        const clang::Stmt* initStmt = nullptr;      // 循环初始化语句 (如 int i = 0)
        ComputeNode::NodeId initNodeId = 0;         // 初始化语句对应的节点ID
        int bodyStartLine = 0;
        int bodyEndLine = 0;
        std::string loopVarName;        // 循环变量名 (如 "i")
        ComputeNode::NodeId anchorNodeId = 0;  // 锚点节点ID
    };

    // 【新增】当前循环信息（用于处理循环变量的直接依赖）
    LoopInfo currentLoopInfo;

    void EnsurePrecedingStatementsBuilt(const clang::Stmt* targetStmt);

    // 【新增】分支信息结构体
    struct BranchInfo {
        ComputeNode::NodeId branchNodeId = 0;       // 分支节点ID
        const clang::Stmt* branchStmt = nullptr;    // if/switch语句
        const clang::Expr* condition = nullptr;     // 条件表达式
        std::string branchType;                     // "THEN", "ELSE", 或空
        int branchLine = 0;                         // 分支所在行号
        int bodyStartLine = 0;                      // 分支体开始行
        int bodyEndLine = 0;                        // 分支体结束行
    };

    // 【新增】当前分支上下文（用于标注分支内的节点）
    BranchInfo currentBranchContext;

    // 深入分析被调用函数
    void AnalyzeCalleeBody(const clang::FunctionDecl* callee,
                           ComputeNode::NodeId callNodeId,
                           const clang::CallExpr* callExpr);

    // 追踪形参使用并连接到实参
    void TraceParameterUsages(
        const clang::FunctionDecl* func,
        const std::map<const clang::ParmVarDecl*, const clang::Expr*>& paramToArg);

    // 递归构建表达式树 (核心方法)
    ComputeNode::NodeId BuildExpressionTree(const clang::Stmt* stmt, int depth);
    // ============================================
    // BuildExpressionTree 辅助函数
    // ============================================
    bool IsControlFlowStmt(const clang::Stmt* stmt);
    bool IsLoopStmt(const clang::Stmt* stmt);
    ComputeNode::NodeId HandleSimpleImplicitCast(
        const clang::ImplicitCastExpr* implCast, int depth);
    const clang::Stmt* FindEnclosingControlFlow(const clang::Stmt* stmt);
    void ApplyLoopContext(std::shared_ptr<ComputeNode> node, const clang::Stmt* stmt);
    void HandleCompoundAssignment(const clang::BinaryOperator* binOp, 
                                  ComputeNode::NodeId nodeId, int depth);
    void HandleAssignment(const clang::BinaryOperator* binOp, 
                         ComputeNode::NodeId nodeId, int depth);
    void HandleNormalBinaryOp(const clang::BinaryOperator* binOp, 
                              ComputeNode::NodeId nodeId, int depth);
    void ProcessBinaryOperator(const clang::BinaryOperator* binOp, 
                               ComputeNode::NodeId nodeId, int depth);
    void ProcessUnaryOperator(const clang::UnaryOperator* unaryOp, 
                              ComputeNode::NodeId nodeId, int depth);
    void ProcessArraySubscript(const clang::ArraySubscriptExpr* arrayExpr, 
                              ComputeNode::NodeId nodeId, int depth);
    void ProcessConstructorExpr(const clang::CXXConstructExpr* ctorExpr, 
                               ComputeNode::NodeId nodeId, int depth);
    void ProcessCallArguments(const clang::CallExpr* callExpr, 
                             ComputeNode::NodeId nodeId, int depth);
    void ProcessCalleeAnalysis(const clang::CallExpr* callExpr, 
                              ComputeNode::NodeId nodeId);
    void ProcessCallExpr(const clang::CallExpr* callExpr, 
                        ComputeNode::NodeId nodeId, int depth);
    void ProcessCastExpr(const clang::CastExpr* castExpr, 
                        ComputeNode::NodeId nodeId, int depth);
    void ProcessMaterializeTemporaryExpr(const clang::MaterializeTemporaryExpr* matTemp, 
                                        ComputeNode::NodeId nodeId, int depth);
    void HandleUnionMemberAccess(ComputeNode::NodeId baseId,
                                ComputeNode::NodeId nodeId,
                                const clang::FieldDecl* fieldDecl,
                                const clang::RecordDecl* recordDecl,
                                const clang::Expr* baseExpr);
    void ProcessMemberExpr(const clang::MemberExpr* memberExpr, 
                          ComputeNode::NodeId nodeId, int depth);
    void ProcessForStmt(const clang::ForStmt* forStmt, 
                       ComputeNode::NodeId nodeId, int depth);
    void ProcessWhileStmt(const clang::WhileStmt* whileStmt, 
                         ComputeNode::NodeId nodeId, int depth);
    void ProcessDoStmt(const clang::DoStmt* doStmt, 
                      ComputeNode::NodeId nodeId, int depth);
    void ProcessConditionalOperator(const clang::ConditionalOperator* condOp, 
                                   ComputeNode::NodeId nodeId, int depth);
    void ProcessReturnStmt(const clang::ReturnStmt* retStmt, 
                          ComputeNode::NodeId nodeId, int depth);
    void ProcessDeclStmt(const clang::DeclStmt* declStmt, 
                        ComputeNode::NodeId nodeId, int depth);
    void ProcessGenericChildren(const clang::Stmt* stmt, 
                               ComputeNode::NodeId nodeId, int depth);
    void ProcessStatementChildren(const clang::Stmt* stmt, 
                                 ComputeNode::NodeId nodeId, int depth);

    // ============================================
    // CreateNodeFromStmt 辅助函数
    // ============================================
    bool DetectCompoundAssignIncrement(const clang::BinaryOperator* binOp,
                                      std::shared_ptr<ComputeNode> node);
    bool DetectAssignmentIncrement(const clang::BinaryOperator* binOp,
                                  std::shared_ptr<ComputeNode> node);
    std::shared_ptr<ComputeNode> CreateBinaryOpNode(const clang::BinaryOperator* binOp);
    std::shared_ptr<ComputeNode> CreateUnaryOpNode(const clang::UnaryOperator* unaryOp);
    std::shared_ptr<ComputeNode> CreateVariableNode(const clang::DeclRefExpr* declRef);
    std::shared_ptr<ComputeNode> CreateIntConstantNode(const clang::IntegerLiteral* intLit);
    std::shared_ptr<ComputeNode> CreateFloatConstantNode(const clang::FloatingLiteral* floatLit);
    std::shared_ptr<ComputeNode> CreateDeclStmtNode(const clang::DeclStmt* declStmt);
    std::shared_ptr<ComputeNode> CreateArrayAccessNode(const clang::ArraySubscriptExpr* arrayExpr);
    std::shared_ptr<ComputeNode> CreateOperatorCallNode(const clang::CXXOperatorCallExpr* opCallExpr);
    std::shared_ptr<ComputeNode> CreateCallExprNode(const clang::CallExpr* callExpr);
    std::shared_ptr<ComputeNode> CreateConstructorNode(const clang::CXXConstructExpr* ctorExpr);
    std::shared_ptr<ComputeNode> CreateMemberAccessNode(const clang::MemberExpr* memberExpr);
    std::shared_ptr<ComputeNode> CreateCastNode(const clang::CastExpr* castExpr, 
                                               const std::string& castType);
    std::shared_ptr<ComputeNode> CreateTempNode(const clang::MaterializeTemporaryExpr* matTemp);
    std::shared_ptr<ComputeNode> CreateReturnNode(const clang::ReturnStmt* retStmt);
    std::shared_ptr<ComputeNode> CreateForLoopNode(const clang::ForStmt* forStmt);
    std::shared_ptr<ComputeNode> CreateWhileLoopNode(const clang::WhileStmt* whileStmt);
    std::shared_ptr<ComputeNode> CreateDoWhileLoopNode(const clang::DoStmt* doStmt);
    std::shared_ptr<ComputeNode> CreateIfBranchNode(const clang::IfStmt* ifStmt);
    std::shared_ptr<ComputeNode> CreateSwitchBranchNode(const clang::SwitchStmt* switchStmt);
    std::shared_ptr<ComputeNode> CreateSelectNode(const clang::ConditionalOperator* condOp);
    std::shared_ptr<ComputeNode> CreateInitListNode(const clang::InitListExpr* initList);
    std::shared_ptr<ComputeNode> CreateCompoundLiteralNode(const clang::CompoundLiteralExpr* compLit);
    void SetContainingFunction(std::shared_ptr<ComputeNode> node, const clang::Stmt* stmt);

    // ============================================
    // TraceAllDefinitionsBackward 辅助函数
    // ============================================
    bool StmtDefinesVariable(const clang::Stmt* stmt, const std::string& varName);
    bool IsLoopVariable(const std::string& varName, int currentLine);
    std::vector<const clang::Stmt*> FindVariableModifications(
        const clang::VarDecl* varDecl, const clang::Stmt* useStmt);
    ComputeNode::NodeId GetVariableNodeFromModStmt(const clang::Stmt* modStmt);
    bool IsLoopCarriedDependency(int modLine, int currentLine);
    void ProcessVariableModification(const clang::Stmt* modStmt, const std::string& varName,
                                     ComputeNode::NodeId varNodeId, const clang::Stmt* useStmt, int depth);
    std::vector<const clang::Stmt*> FindDefinitionsInFunction(
        const std::string& varName, int currentLine, const clang::Stmt* stmt);
    void FindNearestDefinitions(const std::vector<const clang::Stmt*>& filteredDefs,
                               const std::string& varName, int currentLine,
                               const clang::Stmt** nearestBackwardDef, int* nearestBackwardLine,
                               const clang::Stmt** loopCarriedDef, int* maxLoopLine);
    void ProcessDefinitionNode(const clang::Stmt* defStmt, const std::string& varName,
                              ComputeNode::NodeId varNodeId, ComputeEdgeKind edgeKind, int depth);
    void ProcessSingleVariableReference(const clang::DeclRefExpr* varRef, ComputeNode::NodeId varNodeId,
                                        const clang::Stmt* useStmt,
                                        std::set<const clang::VarDecl*>& tracedVars,
                                        std::set<std::pair<std::string, ComputeNode::NodeId>>& tracedVarNodes,
                                        int depth);

    // ============================================
    // AnalyzeCalleeBody 辅助函数
    // ============================================
    void GetInheritedLoopContext(ComputeNode::NodeId callNodeId, ComputeNode::NodeId* inheritedLoopId,
                                std::string* inheritedLoopVar, int* inheritedLoopLine);
    void ClearCalleeStmtMappings(const clang::FunctionDecl* callee);
    void CreateParamArgMapping(const clang::FunctionDecl* callee, const clang::CallExpr* callExpr,
                              ComputeNode::NodeId callNodeId, ComputeNode::NodeId inheritedLoopId,
                              const std::string& inheritedLoopVar, int inheritedLoopLine,
                              std::map<const clang::ParmVarDecl*, const clang::Expr*>& paramToArg,
                              std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId);
    void SetNodeLoopContext(ComputeNode::NodeId nodeId, ComputeNode::NodeId loopId,
                           const std::string& loopVar, int loopLine);
    void ProcessCalleeDeclarations(const std::vector<const clang::DeclStmt*>& declarations,
                                   const clang::FunctionDecl* callee, ComputeNode::NodeId callNodeId,
                                   ComputeNode::NodeId inheritedLoopId, const std::string& inheritedLoopVar,
                                   int inheritedLoopLine);
    void ProcessCalleeAssignments(const std::vector<const clang::BinaryOperator*>& assignments,
                                  const clang::FunctionDecl* callee, ComputeNode::NodeId callNodeId,
                                  ComputeNode::NodeId inheritedLoopId, const std::string& inheritedLoopVar,
                                  int inheritedLoopLine);
    ComputeNode::NodeId FindImplicitReturnValue(const clang::FunctionDecl* callee,
                                                ComputeNode::NodeId callNodeId,
                                                const std::vector<const clang::BinaryOperator*>& assignments);
    void ProcessCalleeReturns(const std::vector<const clang::ReturnStmt*>& returnStmts,
                             const clang::FunctionDecl* callee, ComputeNode::NodeId callNodeId,
                             ComputeNode::NodeId inheritedLoopId, const std::string& inheritedLoopVar,
                             int inheritedLoopLine, std::vector<ComputeNode::NodeId>& returnNodeIds);
    void ProcessImplicitReturn(ComputeNode::NodeId implicitRetNodeId, ComputeNode::NodeId callNodeId,
                              std::vector<ComputeNode::NodeId>& returnNodeIds);
    void TraceReturnValueDependencies(const std::vector<ComputeNode::NodeId>& returnNodeIds);
    void SetCalleeBodyNodeProperties(const std::set<const clang::Stmt*>& calleeStmts,
                                     const clang::FunctionDecl* callee, ComputeNode::NodeId callNodeId,
                                     ComputeNode::NodeId inheritedLoopId, const std::string& inheritedLoopVar,
                                     int inheritedLoopLine);

    // ============================================
    // BuildContainingLoopNode 辅助函数
    // ============================================
    const clang::Stmt* FindContainingLoopStmt(const clang::Stmt* stmt);
    bool TryGetExistingLoopInfo(const clang::Stmt* loopStmt, LoopInfo* info);
    void ComputeLoopBodyRange(const clang::Stmt* body, int* bodyStartLine, int* bodyEndLine);
    LoopInfo BuildForLoopInfo(const clang::ForStmt* forStmt);
    LoopInfo BuildWhileLoopInfo(const clang::WhileStmt* whileStmt);
    LoopInfo BuildDoWhileLoopInfo(const clang::DoStmt* doStmt);


    // 【改进】查找并构建包含语句的循环节点，返回完整的循环信息
    LoopInfo BuildContainingLoopNode(const clang::Stmt* stmt);

    // 【新增】连接循环节点到循环体
    void ConnectLoopToBody(const LoopInfo& loopInfo);

    // 【新增】将循环体内的循环变量连接到Loop节点
    void ConnectLoopVariablesToLoopNode(const LoopInfo& loopInfo);

    // 【新增】连接循环变量的外部初始化到Loop节点
    void ConnectLoopVarInitToLoop(const LoopInfo& loopInfo);

    // 【新增】从for循环中提取循环变量名
    std::string ExtractLoopVarFromFor(const clang::ForStmt* forStmt);

    // 【新增】从条件表达式中提取循环变量名
    std::string ExtractLoopVarFromCondition(const clang::Expr* cond);

    // 【新增】分支相关函数
    // 构建if分支节点
    ComputeNode::NodeId BuildIfBranch(const clang::IfStmt* ifStmt, int depth);

    // 构建分支体（THEN或ELSE）
    ComputeNode::NodeId BuildBranchBody(
        const clang::Stmt* body,
        int depth,
        const std::string& branchType,
        const BranchInfo& parentBranch);

    // 连接分支节点到分支体
    void ConnectBranchToBody(const BranchInfo& branchInfo);

    // 标注分支内的节点
    void MarkNodesInBranch(const BranchInfo& branchInfo);

    // 向后追踪所有变量的定义点
    void TraceAllDefinitionsBackward(const clang::Stmt* stmt, int depth);

    // 追踪union成员的定义
    void TraceUnionMemberDefinitions(const clang::MemberExpr* memberRef,
                                     ComputeNode::NodeId memberNodeId,
                                     const clang::RecordDecl* unionDecl,
                                     int depth);

    // 【新增】连接同一union的不同成员节点（别名关系）
    void ConnectUnionAliases(ComputeNode::NodeId baseId,
                             ComputeNode::NodeId currentMemberId,
                             const clang::RecordDecl* unionDecl,
                             const clang::FieldDecl* currentField);

    const clang::ParmVarDecl* FindParamDeclFromStmt(
    ComputeNode::NodeId nodeId);

    // 获取语句所在的函数
    const clang::FunctionDecl* GetContainingFunction(const clang::Stmt* stmt) const;

    // 追踪参数到调用点的实参
    void TraceParameterToCallSites(const clang::ParmVarDecl* paramDecl,
                                   ComputeNode::NodeId paramNodeId, int depth);

    // 追踪实参到其定义
    void TraceArgumentToDefinition(const clang::Expr* arg,
                                   ComputeNode::NodeId argNodeId,
                                   const clang::FunctionDecl* callerFunc);

    // 遍历所有参数节点追踪到调用点
    void TraceAllParametersToCallSites();

    // 从图节点收集参数
    void CollectParametersFromNodes(
        std::vector<std::pair<const clang::ParmVarDecl*,
                              ComputeNode::NodeId>>& paramsToTrace);

    // 从processedStmts收集参数
    void CollectParametersFromStmts(
        std::vector<std::pair<const clang::ParmVarDecl*,
                              ComputeNode::NodeId>>& paramsToTrace);

    // 检查参数是否已收集
    bool IsParameterAlreadyCollected(
        const clang::ParmVarDecl* paramDecl,
        const std::vector<std::pair<const clang::ParmVarDecl*,
                                    ComputeNode::NodeId>>& paramsToTrace);

    // 标记参数已追踪
    void MarkParameterAsTraced(ComputeNode::NodeId nodeId);

    // 【新增】过滤被后续定义杀死的定义（通用方案）
    std::vector<const clang::Stmt*> FilterKilledDefinitions(
        const std::vector<const clang::Stmt*>& defs,
        const clang::Stmt* useStmt,
        const std::string& varName) const;

    // 【新增】检查定义是否在到达use之前被杀死（向前追踪用）
    bool IsDefinitionKilledBeforeUse(
        const clang::Stmt* defStmt,
        const clang::Stmt* useStmt,
        const std::string& varName) const;

    // 辅助函数：检查中间是否有其他定义
    bool CheckIntermediateDefinitions(
        const clang::Stmt* defStmt,
        const clang::Stmt* useStmt,
        const std::string& varName) const;

    // 【新增】检查语句是否是指针解引用读取
    bool IsPointerDerefStmt(const clang::Stmt* stmt) const;

    // 向前追踪所有变量的使用点
    void TraceAllUsesForward(const clang::Stmt* stmt, int depth);

    // 提取语句中定义的变量
    std::vector<const clang::VarDecl*> ExtractDefinedVariables(
        const clang::Stmt* stmt);

    // 确保控制流已构建
    void EnsureControlFlowBuilt(const clang::Stmt* stmt, int depth);

    // 查找变量的所有使用点
    std::vector<const clang::Stmt*> FindVariableUses(
        const clang::VarDecl* targetVar,
        const clang::FunctionDecl* containingFunc,
        int defLine);

    // 处理单个使用点
    void ProcessSingleUse(
        const clang::Stmt* useStmt,
        ComputeNode::NodeId srcNodeId,
        const std::string& varName,
        const clang::Stmt* defStmt,
        int defLine,
        int depth);

    // 判断是否应该跳过该使用
    bool ShouldSkipUse(
        const clang::Stmt* useStmt,
        const clang::Stmt* defStmt,
        const std::string& varName,
        int useLine,
        int defLine);

    // 处理返回语句中的使用
    void ProcessReturnStmtUse(const clang::Stmt* useStmt, int depth);

    // 检查并递归处理自增自减
    void CheckAndTraceIncrementDecrement(
        const clang::Stmt* useStmt,
        int depth);

    // 向后追踪定义点 (兼容旧API)
    void TraceDefinitionsBackward(const clang::Expr* expr, int depth);

    // 向前追踪使用点 (兼容旧API)
    void TraceUsesForward(const clang::Stmt* stmt,
                          const std::string& varName, int depth);

    // 从Clang AST节点创建计算图节点
    ComputeNode::NodeId CreateNodeFromStmt(const clang::Stmt* stmt);
    ComputeNode::NodeId CreateNodeFromExpr(const clang::Expr* expr);
    ComputeNode::NodeId CreateNodeFromDecl(const clang::Decl* decl);

    // 辅助方法
    OpCode GetOpCodeFromBinaryOp(const clang::BinaryOperator* binOp) const;
    OpCode GetOpCodeFromUnaryOp(const clang::UnaryOperator* unaryOp) const;
    std::string GetOperatorName(OpCode opCode) const;

    // 连接节点
    void ConnectNodes(ComputeNode::NodeId from, ComputeNode::NodeId to,
                      ComputeEdgeKind kind, const std::string& varName = "");

    // 追踪表达式操作数 (现已由BuildExpressionTree替代)
    void TraceExprOperands(const clang::Expr* expr,
                           ComputeNode::NodeId parentId, int depth);

    ComputeNode::NodeId CreateUnaryOpDefNode(
        const clang::UnaryOperator* unaryOp);
    ComputeNode::NodeId CreateBinaryOpDefNode(
        const clang::BinaryOperator* binOp, const std::string& varName);
    ComputeNode::NodeId CreateDeclStmtDefNode(
        const clang::DeclStmt* declStmt, const std::string& varName);
    ComputeNode::NodeId CreateGenericDefNode(
        const clang::Stmt* defStmt, const std::string& varName);
    void SetLoopContextForNode(ComputeNode::NodeId nodeId);

    bool ShouldSkipCalleeAnalysis(const clang::FunctionDecl* callee);
    void InheritLoopContext(
        ComputeNode::NodeId callNodeId,
        ComputeNode::NodeId& inheritedLoopContextId,
        std::string& inheritedLoopContextVar,
        int& inheritedLoopContextLine);
    void ClearCalleeStmts(const clang::FunctionDecl* callee);
    void CreateParamNodesForCallee(
        const clang::FunctionDecl* callee,
        const clang::CallExpr* callExpr,
        ComputeNode::NodeId callNodeId,
        ComputeNode::NodeId inheritedLoopContextId,
        const std::string& inheritedLoopContextVar,
        int inheritedLoopContextLine,
        std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId);

    typedef std::function<void(ComputeNode::NodeId)> SetLoopContextFunc;

    void ProcessReturnStmts(
        const std::vector<const clang::ReturnStmt*>& returns,
        ComputeNode::NodeId callNodeId,
        const clang::FunctionDecl* callee,
        SetLoopContextFunc setLoopContext);
    void PropagateContextToCalleeNodes(
        const clang::FunctionDecl* callee,
        ComputeNode::NodeId callNodeId,
        ComputeNode::NodeId inheritedLoopContextId,
        const std::string& inheritedLoopContextVar,
        int inheritedLoopContextLine);

    // 辅助函数：注册参数引用
    void RegisterParamRefsInCallee(
        const clang::FunctionDecl* callee,
        const std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId);

    // 辅助函数：处理被调用函数体内的语句
    void ProcessCalleeBodyStmts(
        const clang::FunctionDecl* callee,
        ComputeNode::NodeId callNodeId,
        ComputeNode::NodeId inheritedLoopContextId,
        const std::string& inheritedLoopContextVar,
        int inheritedLoopContextLine);

    // 辅助函数：查找隐式返回值
    ComputeNode::NodeId FindImplicitReturnValue(
        const clang::FunctionDecl* callee,
        ComputeNode::NodeId callNodeId);


};

    // 辅助函数：查找隐式返回值
    ComputeNode::NodeId FindImplicitReturnValue(
        const clang::FunctionDecl* callee,
        ComputeNode::NodeId callNodeId);

// ============================================
// 计算图合并器
// ============================================
class ComputeGraphMerger {
public:
    // 合并两个图
    static std::shared_ptr<ComputeGraph> Merge(
        const ComputeGraph& g1, const ComputeGraph& g2);

    // 合并多个图
    static std::shared_ptr<ComputeGraph> MergeAll(
        const std::vector<std::shared_ptr<ComputeGraph>>& graphs);

    // 检查两个图是否有重叠 (共享AST节点)
    static bool HasOverlap(const ComputeGraph& g1, const ComputeGraph& g2);

    // 查找两个图的公共子图
    static std::shared_ptr<ComputeGraph> FindCommonSubgraph(
        const ComputeGraph& g1, const ComputeGraph& g2);

    // 去重：从图集中移除同构图
    static void DeduplicateGraphSet(ComputeGraphSet& graphSet);

    // 合并重叠的图
    static void MergeOverlappingGraphs(ComputeGraphSet& graphSet);

private:
    // 检查两个节点是否可以合并
    static bool CanMergeNodes(const ComputeNode& n1, const ComputeNode& n2);

    // 复制节点属性
    static void CopyNodeProperties(const ComputeNode* src, ComputeNode* dst);
};

// 全局辅助函数：合并重叠的图
void MergeOverlappingGraphs(ComputeGraphSet& graphSet);

// ============================================
// 模式匹配器 (用于向量化规则匹配)
// ============================================
struct PatternNode {
    ComputeNodeKind kind;
    OpCode opCode = OpCode::Unknown;
    std::string constraint;     // 约束条件
    int captureId = -1;         // 捕获ID (用于在重写中引用)

    std::vector<int> inputPatternIds;   // 输入模式节点ID
};

struct RewritePattern {
    std::string name;
    std::vector<PatternNode> pattern;       // 源模式
    std::vector<PatternNode> replacement;   // 替换模式

    // 模式条件
    std::function<bool(const std::map<int, ComputeNode::NodeId>&)> condition;
};

class PatternMatcher {
public:
    // 注册重写模式
    void RegisterPattern(const RewritePattern& pattern);

    // 在图中查找匹配的模式
    std::vector<std::map<int, ComputeNode::NodeId>> FindMatches(
        const ComputeGraph& graph, const std::string& patternName);

    // 应用重写规则
    std::shared_ptr<ComputeGraph> ApplyRewrite(
        const ComputeGraph& graph,
        const std::string& patternName,
        const std::map<int, ComputeNode::NodeId>& match);

    // 获取所有已注册的模式
    std::vector<std::string> GetRegisteredPatterns() const;

private:
    std::map<std::string, RewritePattern> patterns;

    bool MatchNode(const ComputeGraph& graph,
                   ComputeNode::NodeId nodeId,
                   const PatternNode& patternNode,
                   std::map<int, ComputeNode::NodeId>& bindings);
};


    // 辅助函数：注册参数引用
    void RegisterParamRefsInCallee(
        const clang::FunctionDecl* callee,
        const std::map<const clang::ParmVarDecl*, ComputeNode::NodeId>& paramToNodeId);

    // 辅助函数：处理被调用函数体内的语句
    void ProcessCalleeBodyStmts(
        const clang::FunctionDecl* callee,
        ComputeNode::NodeId callNodeId,
        ComputeNode::NodeId inheritedLoopContextId,
        const std::string& inheritedLoopContextVar,
        int inheritedLoopContextLine);


// ============================================
// 辅助函数
// ============================================
std::string OpCodeToString(OpCode op);
OpCode StringToOpCode(const std::string& str);
std::string ComputeNodeKindToString(ComputeNodeKind kind);
std::string ComputeEdgeKindToString(ComputeEdgeKind kind);

    class UnionDefFinder : public clang::RecursiveASTVisitor<UnionDefFinder> {
    public:
        const clang::VarDecl* targetBase;
        const clang::RecordDecl* unionDecl;
        std::vector<const clang::BinaryOperator*> defs;
        std::vector<const clang::DeclStmt*> declDefs;

        UnionDefFinder(const clang::VarDecl* base, const clang::RecordDecl* ud)
            : targetBase(base), unionDecl(ud) {}

        bool VisitBinaryOperator(clang::BinaryOperator* binOp) {
            if (!binOp->isAssignmentOp()) {
                return true;
            }

            const clang::Expr* lhs = binOp->getLHS()->IgnoreParenImpCasts();
            const clang::MemberExpr* memberExpr =
                llvm::dyn_cast<clang::MemberExpr>(lhs);

            if (memberExpr) {
                const clang::Expr* base =
                    memberExpr->getBase()->IgnoreParenImpCasts();
                const clang::DeclRefExpr* declRef =
                    llvm::dyn_cast<clang::DeclRefExpr>(base);

                if (declRef && declRef->getDecl() == targetBase) {
                    defs.push_back(binOp);
                }
            }
            return true;
        }

        bool VisitDeclStmt(clang::DeclStmt* declStmt) {
            for (clang::Decl* decl : declStmt->decls()) {
                if (decl == targetBase) {
                    declDefs.push_back(declStmt);
                }
            }
            return true;
        }
    };

    class CallSiteFinder : public clang::RecursiveASTVisitor<CallSiteFinder> {
    public:
        const clang::FunctionDecl* targetFunc;
        std::vector<const clang::CallExpr*> callSites;
        clang::SourceManager* sourceManager;

        explicit CallSiteFinder(const clang::FunctionDecl* f)
            : targetFunc(f), sourceManager(nullptr) {}

        void SetSourceManager(clang::SourceManager* sm) {
            sourceManager = sm;
        }

        bool shouldVisitImplicitCode() const {
            return false;
        }

        bool shouldVisitTemplateInstantiations() const {
            return false;
        }

        bool TraverseDecl(clang::Decl* D) {
            if (!D) {
                return true;
            }

            if (sourceManager) {
                clang::SourceLocation loc = D->getLocation();
                if (loc.isValid() && sourceManager->isInSystemHeader(loc)) {
                    return true;
                }
            }

            if (D->isImplicit()) {
                return true;
            }

            return clang::RecursiveASTVisitor<CallSiteFinder>::TraverseDecl(D);
        }

        bool TraverseType(clang::QualType T) {
            return true;
        }

        bool TraverseTypeLoc(clang::TypeLoc TL) {
            return true;
        }

        bool VisitCallExpr(clang::CallExpr* call) {
            if (!call) {
                return true;
            }

            if (sourceManager) {
                clang::SourceLocation loc = call->getBeginLoc();
                if (loc.isValid() && sourceManager->isInSystemHeader(loc)) {
                    return true;
                }
            }

            const clang::FunctionDecl* callee = call->getDirectCallee();
            if (callee && callee->getCanonicalDecl() ==
                         targetFunc->getCanonicalDecl()) {
                callSites.push_back(call);
                         }
            return true;
        }
    };

class IntermediateDefFinder : public clang::RecursiveASTVisitor<IntermediateDefFinder> {
    public:
        std::string targetVar;
        int defLine;
        int useLine;
        bool foundIntermediate;
        clang::ASTContext& ctx;

        IntermediateDefFinder(const std::string& var, int dLine, int uLine,
                             clang::ASTContext& c)
            : targetVar(var), defLine(dLine), useLine(uLine),
              foundIntermediate(false), ctx(c) {}

        int GetLine(const clang::Stmt* s) {
            return ctx.getSourceManager().getSpellingLineNumber(s->getBeginLoc());
        }

        bool VisitBinaryOperator(clang::BinaryOperator* binOp) {
            if (!binOp->isAssignmentOp()) {
                return true;
            }

            const clang::Expr* lhsExpr = binOp->getLHS()->IgnoreParenImpCasts();
            const clang::DeclRefExpr* lhs =
                llvm::dyn_cast<clang::DeclRefExpr>(lhsExpr);

            if (lhs && lhs->getDecl()->getNameAsString() == targetVar) {
                int line = GetLine(binOp);
                if (line > defLine && line < useLine) {
                    foundIntermediate = true;
                    return false;
                }
            }
            return true;
        }

        bool VisitDeclStmt(clang::DeclStmt* declStmt) {
            for (clang::Decl* decl : declStmt->decls()) {
                const clang::VarDecl* varDecl =
                    llvm::dyn_cast<clang::VarDecl>(decl);

                if (varDecl && varDecl->getNameAsString() == targetVar) {
                    int line = GetLine(declStmt);
                    if (line > defLine && line < useLine) {
                        foundIntermediate = true;
                        return false;
                    }
                }
            }
            return true;
        }

        bool VisitUnaryOperator(clang::UnaryOperator* unaryOp) {
            if (!unaryOp->isIncrementDecrementOp()) {
                return true;
            }

            const clang::Expr* operandExpr =
                unaryOp->getSubExpr()->IgnoreParenImpCasts();
            const clang::DeclRefExpr* operand =
                llvm::dyn_cast<clang::DeclRefExpr>(operandExpr);

            if (operand && operand->getDecl()->getNameAsString() == targetVar) {
                int line = GetLine(unaryOp);
                if (line > defLine && line < useLine) {
                    foundIntermediate = true;
                    return false;
                }
            }
            return true;
        }
    };

    class VarRefExtractor : public clang::RecursiveASTVisitor<VarRefExtractor> {
    public:
        std::vector<const clang::VarDecl*> varDecls;

        bool VisitDeclRefExpr(clang::DeclRefExpr* ref) {
            const clang::VarDecl* varDecl =
                llvm::dyn_cast<clang::VarDecl>(ref->getDecl());

            if (varDecl) {
                varDecls.push_back(varDecl);
            }
            return true;
        }
    };

    class DeclFinder : public clang::RecursiveASTVisitor<DeclFinder> {
    public:
        const clang::VarDecl* targetDecl;
        const clang::DeclStmt* foundDeclStmt;

        explicit DeclFinder(const clang::VarDecl* d)
            : targetDecl(d), foundDeclStmt(nullptr) {}

        bool VisitDeclStmt(clang::DeclStmt* declStmt) {
            for (clang::Decl* decl : declStmt->decls()) {
                if (decl == targetDecl) {
                    foundDeclStmt = declStmt;
                    return false;
                }
            }
            return true;
        }
    };

    class StmtCollector : public clang::RecursiveASTVisitor<StmtCollector> {
    public:
        std::set<const clang::Stmt*> stmts;

        bool VisitStmt(clang::Stmt* s) {
            stmts.insert(s);
            return true;
        }
    };
} // namespace compute_graph

#endif // COMPUTE_GRAPH_H