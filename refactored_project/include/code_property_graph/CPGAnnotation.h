/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#ifndef CPG_ANNOTATION_V2_H
#define CPG_ANNOTATION_V2_H

#include "code_property_graph/CPGBase.h"
#include "clang/Basic/SourceManager.h"  // 必须包含，用于 isInSystemHeader

namespace cpg {

// ============================================
// CPG上下文
// ============================================
class CPGContext {
public:
    explicit CPGContext(clang::ASTContext& ctx);

    // ============================================
    // ICFG接口
    // ============================================
    ICFGNode* GetICFGNode(const clang::Stmt* stmt) const;
    ICFGNode* GetFunctionEntry(const clang::FunctionDecl* func) const;
    ICFGNode* GetFunctionExit(const clang::FunctionDecl* func) const;
    std::vector<ICFGNode*> GetSuccessors(ICFGNode* node) const;
    std::vector<ICFGNode*> GetPredecessors(ICFGNode* node) const;
    std::vector<std::pair<ICFGNode*, ICFGEdgeKind>>
        GetSuccessorsWithEdgeKind(ICFGNode* node) const;

    // ============================================
    // PDG接口
    // ============================================
    PDGNode* GetPDGNode(const clang::Stmt* stmt) const;
    std::vector<DataDependency> GetDataDependencies(const clang::Stmt* stmt) const;
    std::vector<ControlDependency> GetControlDependencies(const clang::Stmt* stmt) const;
    std::set<const clang::Stmt*> GetDefinitions(const clang::Stmt* useStmt,
                                                 const std::string& varName) const;
    std::set<const clang::Stmt*> GetUses(const clang::Stmt* defStmt,
                                          const std::string& varName) const;

    // ============================================
    // 路径查询
    // ============================================
    bool HasDataFlowPath(const clang::Stmt* source, const clang::Stmt* sink,
                         const std::string& varName = "") const;
    bool HasControlFlowPath(const clang::Stmt* source, const clang::Stmt* sink) const;
    std::vector<std::vector<ICFGNode*>>
        FindAllPaths(ICFGNode* source, ICFGNode* sink, int maxDepth = 100) const;

    // ============================================
    // 辅助功能
    // ============================================
    const clang::FunctionDecl* GetContainingFunction(const clang::Stmt* stmt) const;
    const clang::CFG* GetCFG(const clang::FunctionDecl* func) const;

    // ============================================
    // 可视化接口
    // ============================================
    void DumpICFG(const clang::FunctionDecl* func) const;
    void DumpPDG(const clang::FunctionDecl* func) const;
    void DumpCPG(const clang::FunctionDecl* func) const;
    void DumpInterproceduralEdges() const;
    void VisualizeICFG(const clang::FunctionDecl* func,
                       const std::string& outputPath = ".") const;  // 包含调用链的完整ICFG
    void VisualizePDG(const clang::FunctionDecl* func,
                      const std::string& outputPath = ".") const;
    void VisualizeCPG(const clang::FunctionDecl* func,
                      const std::string& outputPath = ".") const;
    void DumpNode(ICFGNode* node) const;
    void DumpNode(PDGNode* node) const;

    // ============================================
    // 统计信息
    // ============================================
    void PrintStatistics() const;

    // ============================================
    // 构建接口
    // ============================================
    void BuildCPG(const clang::FunctionDecl* func);
    void BuildICFGForTranslationUnit();

    // ============================================
    // 数据流分析接口
    // ============================================
    std::set<std::string> ExtractVariables(const clang::Expr* expr) const;
    std::vector<const clang::Stmt*> TraceVariableDefinitions(
        const clang::Expr* expr, int maxDepth = 10) const;
    std::vector<const clang::Stmt*> TraceVariableDefinitionsInterprocedural(
        const clang::Expr* expr, int maxDepth = 10) const;
    std::vector<const clang::Stmt*> TraceVariableUsesInterprocedural(
        const clang::Stmt* defStmt, const std::string& varName = "",
        int maxDepth = 10) const;
    const clang::Stmt* GetContainingStmt(const clang::Expr* expr) const;
    const clang::Expr* GetArgumentAtCallSite(const clang::CallExpr* callExpr,
                                              unsigned paramIndex) const;
    std::vector<const clang::Stmt*> GetParameterUsages(
        const clang::ParmVarDecl* param) const;

    // 【新增】从缓存直接获取（避免重复计算）
    std::set<std::string> GetUsedVarsCached(const clang::Stmt* stmt) const;
    std::set<std::string> GetDefinedVarsCached(const clang::Stmt* stmt) const;

    // ============================================
    // 上下文敏感接口
    // ============================================
    PDGNode* GetPDGNodeInContext(const clang::Stmt* stmt,
                                  const CallContext& context) const;
    std::vector<DataDependency>
        GetDataDependenciesOnPath(const clang::Stmt* stmt,
                                  const PathCondition& path) const;

    using CallGraphVisitor = std::function<void(const clang::FunctionDecl*,
                                                 const CallContext&)>;
    void TraverseCallGraphContextSensitive(const clang::FunctionDecl* entry,
                                           CallGraphVisitor visitor,
                                           int maxDepth = 10) const;

    // 调用图遍历辅助方法
    void ProcessCallSiteContextSensitive(
        const clang::CallExpr* call,
        const CallContext& context,
        int depth, int maxDepth,
        CallGraphVisitor& visitor) const;

    void TraverseCallGraphDFS(
        const clang::FunctionDecl* func,
        const CallContext& context,
        int depth, int maxDepth,
        CallGraphVisitor& visitor) const;

    // 辅助函数：处理单个 CFGBlock
    void ProcessCFGBlock(
            const clang::CFGBlock* block,
            const clang::FunctionDecl* func,
            std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
            std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode);

    ICFGNode* ProcessCFGElement(
        const clang::CFGElement& elem, const clang::FunctionDecl* func,
        const clang::CFGBlock* block);

    void ConnectBlockSuccessors(
        const clang::CFGBlock* block, ICFGNode* lastNode,
        const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode);

    ICFGEdgeKind DetermineEdgeKind(
            const clang::CFGBlock* block,
            int succIndex) const;

    using DefsMap = std::map<std::string, std::set<const clang::Stmt*>>;
    using BlockDefsMap = std::map<const clang::CFGBlock*, DefsMap>;

    // Reaching Definitions 辅助方法
    DefsMap ComputeBlockIn(const clang::CFGBlock* block,
                           const BlockDefsMap& blockOut) const;

    void ApplyKillGen(const clang::CFGBlock* block,
                      DefsMap& currentDefs,
                      ReachingDefsInfo& info);

    bool ProcessBlockReachingDefs(const clang::CFGBlock* block,
                                  BlockDefsMap& blockOut,
                                  ReachingDefsInfo& info);

    // 类型别名
    using BlockSet = std::set<const clang::CFGBlock*>;
    using PostDomMap = std::map<const clang::CFGBlock*, BlockSet>;

    // Post Dominator 辅助方法
    BlockSet IntersectWithBlock(const BlockSet& set1, const BlockSet& set2,
                                const clang::CFGBlock* block) const;

    BlockSet ComputeNewPostDom(const clang::CFGBlock* block,
                               const PostDomMap& postDom) const;

    bool UpdateBlockPostDom(const clang::CFGBlock* block,
                            const clang::CFGBlock* exitBlock,
                            PostDomMap& postDom);

    // ICFG 入口出口连接辅助方法
    void ConnectEntryNode(
        const clang::CFG* cfg, ICFGNode* entryNode,
        const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode);

    void ConnectExitNode(
        const clang::CFG* cfg, ICFGNode* exitNode,
        const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode);

    // 控制依赖辅助方法
    bool IsPostDominatedBy(const clang::CFGBlock* current,
                           const clang::CFGBlock* block,
                           const PostDomMap& postDom) const;

    void AddControlDepsForBlock(const clang::CFGBlock* current,
                                const clang::Stmt* term, bool branchValue,
                                const clang::FunctionDecl* func);

    void EnsurePDGNode(const clang::Stmt* s,
                       const clang::FunctionDecl* func);

    void EnqueueSuccessors(const clang::CFGBlock* current,
                           std::queue<const clang::CFGBlock*>& worklist,
                           std::set<const clang::CFGBlock*>& visited);

    // 调用图构建辅助方法
    const clang::FunctionDecl* FindContainingFunctionForCall(
        const clang::CallExpr* call) const;

    bool ContainsCallExpr(
        const std::vector<std::unique_ptr<ICFGNode>>& nodes,
        const clang::CallExpr* call) const;

    void RegisterCallSite(clang::CallExpr* call);

private:
    clang::ASTContext& astContext;

    // ICFG相关
    std::map<const clang::FunctionDecl*,
             std::vector<std::unique_ptr<ICFGNode>>> icfgNodes;
    std::map<const clang::Stmt*, ICFGNode*> stmtToICFGNode;
    std::map<const clang::FunctionDecl*, ICFGNode*> funcEntries;
    std::map<const clang::FunctionDecl*, ICFGNode*> funcExits;

    // PDG相关
    std::map<const clang::Stmt*, std::unique_ptr<PDGNode>> pdgNodes;

    // Reaching Definitions分析
    std::map<const clang::FunctionDecl*, ReachingDefsInfo> reachingDefsMap;

    // CFG缓存
    std::map<const clang::FunctionDecl*, std::unique_ptr<clang::CFG>> cfgCache;

    // 调用图
    std::map<const clang::FunctionDecl*, std::set<const clang::CallExpr*>> callSites;
    std::map<const clang::CallExpr*, const clang::FunctionDecl*> callTargets;

    // 预留：上下文敏感分析
    std::map<CallContext, std::unique_ptr<PDGNode>> contextSensitivePDG;
    // ============================================
    // 内部构建方法
    // ============================================
    void BuildICFG(const clang::FunctionDecl* func);
    void BuildCallGraph();
    void LinkCallSites();
    ICFGNode* CreateICFGNode(ICFGNodeKind kind, const clang::FunctionDecl* func);
    void AddICFGEdge(ICFGNode* from, ICFGNode* to, ICFGEdgeKind kind);

    // ICFG构建辅助方法
    void BuildICFGNodes(const clang::FunctionDecl* func, const clang::CFG* cfg,
                        std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
                        std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode);
    void ConnectICFGBlocks(const clang::CFG* cfg,
                           const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
                           const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode);
    void ConnectICFGEntryExit(const clang::FunctionDecl* func, const clang::CFG* cfg,
                              ICFGNode* entryNode, ICFGNode* exitNode,
                              const std::map<const clang::CFGBlock*, ICFGNode*>& blockFirstNode,
                              const std::map<const clang::CFGBlock*, ICFGNode*>& blockLastNode);

    // PDG构建
    void BuildPDG(const clang::FunctionDecl* func);
    void ComputeReachingDefinitions(const clang::FunctionDecl* func);
    void ComputeDataDependencies(const clang::FunctionDecl* func);
    void ComputeControlDependencies(const clang::FunctionDecl* func);
    void ComputePostDominators(const clang::FunctionDecl* func,
                               std::map<const clang::CFGBlock*,
                                       std::set<const clang::CFGBlock*>>& postDom);

    // Reaching Definitions辅助方法
    void CollectDefsAndUses(const clang::CFG* cfg, ReachingDefsInfo& info);
    void IterateReachingDefs(const clang::CFG* cfg, ReachingDefsInfo& info);

    // 控制依赖辅助方法
    void ProcessControlBranch(const clang::CFGBlock* block, const clang::Stmt* term,
                              const clang::CFGBlock* succBlock, bool branchValue,
                              const std::map<const clang::CFGBlock*,
                                            std::set<const clang::CFGBlock*>>& postDom);

    // 参数节点辅助方法
    ICFGNode* FindFormalInNode(const clang::FunctionDecl* callee, int paramIndex) const;
    std::string GetArgumentName(const clang::Expr* arg) const;

    // 可视化辅助
    std::string GetStmtSource(const clang::Stmt* stmt) const;
    std::string EscapeForDot(const std::string& str) const;
    void ExportICFGDotFile(const clang::FunctionDecl* func,
                           const std::string& filename) const;
    void CollectCalleeFunctions(
    const clang::FunctionDecl* func,
    std::set<const clang::FunctionDecl*>& result) const;

    void WriteICFGSubgraphNodes(
        llvm::raw_fd_ostream& out,
        const clang::FunctionDecl* func,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WriteICFGSubgraph(
        llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
        const clang::FunctionDecl* entryFunc,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WriteICFGFunctionEdges(
        llvm::raw_fd_ostream& out,
        const clang::FunctionDecl* func,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void ExportICFGWithCalleesDotFile(const clang::FunctionDecl* func,
                                       const std::string& filename) const;
    void ExportPDGDotFile(const clang::FunctionDecl* func,
                          const std::string& filename) const;
    void AssignGlobalNodeIdsForCPG(
    const std::set<const clang::FunctionDecl*>& funcsToInclude,
    std::map<ICFGNode*, int>& globalNodeIds,
    std::map<const clang::Stmt*, int>& globalStmtToNodeId) const;

    void WriteCPGSubgraphNodes(
        llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WriteCPGSubgraph(
        llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
        const clang::FunctionDecl* entryFunc,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WriteNodeControlFlowEdges(
        llvm::raw_fd_ostream& out,
        ICFGNode* node, int fromId,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WriteCPGControlFlowEdges(
        llvm::raw_fd_ostream& out,
        const std::set<const clang::FunctionDecl*>& funcsToInclude,
        const std::map<ICFGNode*, int>& globalNodeIds) const;

    void WritePDGNodeDataDeps(
        llvm::raw_fd_ostream& out,
        const PDGNode* pdgNode, int sinkId,
        const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const;

    void WriteCPGDataDependencyEdges(
        llvm::raw_fd_ostream& out,
        const std::set<const clang::FunctionDecl*>& funcsToInclude,
        const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const;

    void WritePDGNodeControlDeps(
        llvm::raw_fd_ostream& out,
        const PDGNode* pdgNode, int depId,
        const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const;

    void WriteCPGControlDependencyEdges(
        llvm::raw_fd_ostream& out,
        const std::set<const clang::FunctionDecl*>& funcsToInclude,
        const std::map<const clang::Stmt*, int>& globalStmtToNodeId) const;

    void ExportCPGDotFile(const clang::FunctionDecl* func,
                          const std::string& filename) const;

    // DOT导出辅助方法
    void WriteICFGNodes(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                        std::map<ICFGNode*, int>& nodeIds) const;
    void WriteICFGEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                        const std::map<ICFGNode*, int>& nodeIds) const;
    void WriteEdgeAttributes(llvm::raw_fd_ostream& out, ICFGEdgeKind kind) const;
    std::string GetNodeColor(ICFGNodeKind kind) const;
    void WritePDGNodes(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                       std::map<const clang::Stmt*, int>& nodeIds) const;
    void WritePDGEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                       const std::map<const clang::Stmt*, int>& nodeIds) const;
    void WritePDGDataEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                           const std::map<const clang::Stmt*, int>& nodeIds) const;
    void WritePDGControlEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                              const std::map<const clang::Stmt*, int>& nodeIds) const;

    // CPG DOT导出辅助方法
    void BuildCPGNodeMappings(const clang::FunctionDecl* func,
                              std::map<ICFGNode*, int>& icfgNodeIds,
                              std::map<const clang::Stmt*, int>& stmtToNodeId) const;
    void WriteCPGNodes(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                       const std::map<ICFGNode*, int>& icfgNodeIds) const;
    void WriteCPGControlFlowEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                                  const std::map<ICFGNode*, int>& icfgNodeIds) const;
    void WriteCPGDataDepEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                              const std::map<const clang::Stmt*, int>& stmtToNodeId) const;
    void WriteCPGControlDepEdges(llvm::raw_fd_ostream& out, const clang::FunctionDecl* func,
                                 const std::map<const clang::Stmt*, int>& stmtToNodeId) const;

    // 调用图链接辅助方法
    void LinkSingleCallSite(const clang::FunctionDecl* caller,
                            const clang::CallExpr* callExpr);
    void CreateParameterNodes(const clang::FunctionDecl* caller,
                               const clang::FunctionDecl* callee,
                               const clang::CallExpr* callExpr,
                               ICFGNode* callNode);

    // Post dominator辅助方法
    void IteratePostDominators(const clang::CFG* cfg,
                                const clang::CFGBlock* exitBlock,
                                std::map<const clang::CFGBlock*,
                                        std::set<const clang::CFGBlock*>>& postDom);

    void TraceDefinitionsForVar(const std::string& varName,
                                 std::queue<std::pair<const clang::Stmt*, int>>& worklist,
                                 std::set<const clang::Stmt*>& visited,
                                 std::vector<const clang::Stmt*>& result,
                                 int maxDepth) const;
    void ProcessInterproceduralBackwardTrace(
        const clang::Expr* expr,
        std::queue<InterproceduralWorkItem>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result,
        int maxDepth) const;
    void ProcessLocalDefinitions(const clang::Stmt* current,
                                  const clang::FunctionDecl* currentFunc,
                                  const std::string& varName, int depth,
                                  std::queue<InterproceduralWorkItem>& worklist,
                                  std::set<const clang::Stmt*>& visited,
                                  std::vector<const clang::Stmt*>& result) const;
    void ProcessParameterBackward(const clang::Expr* expr,
                                   const clang::FunctionDecl* currentFunc,
                                   const std::string& varName, int depth,
                                   std::queue<InterproceduralWorkItem>& worklist,
                                   std::set<const clang::Stmt*>& visited,
                                   std::vector<const clang::Stmt*>& result) const;
    void TraceParameterBackward(const clang::FunctionDecl* currentFunc,
                                 unsigned paramIndex,
                                 const std::string& varName, int depth,
                                 std::queue<InterproceduralWorkItem>& worklist,
                                 std::set<const clang::Stmt*>& visited,
                                 std::vector<const clang::Stmt*>& result) const;
    void ProcessInterproceduralForwardTrace(
        std::queue<ForwardWorkItem>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result,
        int maxDepth) const;
    void CollectLocalUses(const clang::Stmt* currentDef,
                          const clang::ParmVarDecl* currentParam,
                          const std::string& currentVar,
                          std::vector<const clang::Stmt*>& localUses) const;
    void ProcessForwardUse(const clang::Stmt* useStmt,
                           const std::string& currentVar,
                           const clang::FunctionDecl* currentFunc, int depth,
                           std::queue<ForwardWorkItem>& worklist) const;
    void ProcessForwardCallSite(const clang::CallExpr* callExpr,
                                 const std::string& currentVar, int depth,
                                 std::queue<ForwardWorkItem>& worklist) const;
    void ProcessForwardAssignment(const clang::BinaryOperator* binOp,
                                   const std::string& currentVar,
                                   const clang::FunctionDecl* currentFunc, int depth,
                                   std::queue<ForwardWorkItem>& worklist) const;
    void ProcessForwardDeclStmt(const clang::DeclStmt* declStmt,
                                 const std::string& currentVar,
                                 const clang::FunctionDecl* currentFunc, int depth,
                                 std::queue<ForwardWorkItem>& worklist) const;

    void CollectUsedVarsFromAssignment(
    const clang::BinaryOperator* binOp,
    std::set<std::string>& vars) const;

    void CollectUsedVarsFromDeclStmt(
        const clang::DeclStmt* declStmt,
        std::set<std::string>& vars) const;

    // 辅助函数
    std::set<std::string> GetUsedVars(const clang::Stmt* stmt) const;
    std::set<std::string> GetDefinedVars(const clang::Stmt* stmt) const;

    // 数据流追踪辅助方法
    void ProcessDefinitionStmt(
        const clang::Stmt* defStmt, int depth,
        std::queue<std::pair<const clang::Stmt*, int>>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result) const;

    void ProcessDefinitionsRound(
        const clang::Stmt* current,
        int depth, const std::string& varName,
        std::queue<std::pair<const clang::Stmt*, int>>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result) const;

    bool IsCallToFunction(const clang::CallExpr* callExpr,
                          const clang::FunctionDecl* targetFunc) const;

    void ProcessArgumentBackward(
        const clang::Expr* arg, const clang::CallExpr* callExpr,
        const clang::FunctionDecl* caller, int depth,
        std::queue<InterproceduralWorkItem>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result) const;

    void ProcessCallSiteBackward(
        const clang::CallExpr* callExpr,
        const clang::FunctionDecl* caller,
        const clang::FunctionDecl* currentFunc,
        unsigned paramIndex, int depth,
        std::queue<InterproceduralWorkItem>& worklist,
        std::set<const clang::Stmt*>& visited,
        std::vector<const clang::Stmt*>& result) const;

    void ProcessVarDeclForward(
        const clang::VarDecl* varDecl,
        const clang::DeclStmt* declStmt,
        const std::string& currentVar,
        const clang::FunctionDecl* currentFunc,
        int depth,
        std::queue<ForwardWorkItem>& worklist) const;

    // 数据流路径查询辅助方法
    void EnqueueVariableUses(
        const clang::Stmt* current,
        const std::string& var,
        std::queue<const clang::Stmt*>& worklist,
        std::set<const clang::Stmt*>& visited) const;

    void ProcessDefinedVarsForPath(
        const clang::Stmt* current,
        const std::string& varName,
        std::queue<const clang::Stmt*>& worklist,
        std::set<const clang::Stmt*>& visited) const;

    void ExploreSuccessors(
        ICFGNode* node, ICFGNode* sink,
        int depth, int maxDepth,
        std::vector<ICFGNode*>& currentPath,
        std::set<ICFGNode*>& visited,
        std::vector<std::vector<ICFGNode*>>& allPaths) const;

    void FindPathsDFS(
        ICFGNode* node, ICFGNode* sink,
        int depth, int maxDepth,
        std::vector<ICFGNode*>& currentPath,
        std::set<ICFGNode*>& visited,
        std::vector<std::vector<ICFGNode*>>& allPaths) const;

    void ExtractDefinedVarFromAssignment(
        const clang::BinaryOperator* binOp,
        std::set<std::string>& vars) const;

    void ExtractDefinedVarsFromDeclStmt(
        const clang::DeclStmt* declStmt,
        std::set<std::string>& vars) const;

    void ExtractDefinedVarFromUnaryOp(
        const clang::UnaryOperator* unaryOp,
        std::set<std::string>& vars) const;

    friend class CPGBuilder;
};

// ============================================
// CPG构建器
// ============================================
class CPGBuilder {
public:
    static void BuildForTranslationUnit(clang::ASTContext& astCtx, CPGContext& cpgCtx);
    static void BuildForFunction(const clang::FunctionDecl* func, CPGContext& cpgCtx);
};

class CallGraphBuilder : public clang::RecursiveASTVisitor<CallGraphBuilder> {
public:
    CPGContext& ctx;
    clang::SourceManager* sourceManager;

    explicit CallGraphBuilder(CPGContext& c)
        : ctx(c), sourceManager(nullptr) {}

    void SetSourceManager(clang::SourceManager* sm)
    {
        sourceManager = sm;
    }

    // 不遍历隐式代码（如隐式构造函数等）
    bool shouldVisitImplicitCode() const
    {
        return false;
    }

    // 不遍历模板实例化
    bool shouldVisitTemplateInstantiations() const { return false; }

    // 控制Decl遍历，跳过系统头文件
    bool TraverseDecl(clang::Decl* D)
    {
        if (!D) {
            return true;
        }

        // 检查是否在系统头文件中
        if (sourceManager) {
            clang::SourceLocation loc = D->getLocation();
            if (loc.isValid() && sourceManager->isInSystemHeader(loc)) {
                return true;  // 跳过系统头文件中的声明
            }
        }

        // 跳过隐式声明
        if (D->isImplicit()) {
            return true;
        }

        return clang::RecursiveASTVisitor<CallGraphBuilder>::TraverseDecl(D);
    }

    // 控制类型遍历，避免进入复杂模板。 跳过类型遍历，我们只关心调用表达式
    bool TraverseType(clang::QualType T)
    {
        return true;
    }

    // 控制TypeLoc遍历
    bool TraverseTypeLoc(clang::TypeLoc TL)
    {
        return true;
    }

    // 控制NestedNameSpecifier遍历
    bool TraverseNestedNameSpecifier(clang::NestedNameSpecifier* NNS)
    {
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* call)
    {
        if (!call) return true;

        // 检查调用是否在系统头文件中
        if (sourceManager) {
            clang::SourceLocation loc = call->getBeginLoc();
            if (loc.isValid() && sourceManager->isInSystemHeader(loc)) {
                return true;  // 跳过系统头文件中的调用
            }
        }

        ctx.RegisterCallSite(call);
        return true;
    }
};

class ParamUsageFinder : public clang::RecursiveASTVisitor<ParamUsageFinder> {
public:
    const clang::ParmVarDecl* targetParam;
    std::vector<const clang::Stmt*> foundUsages;

    explicit ParamUsageFinder(const clang::ParmVarDecl* p) : targetParam(p) {}

    bool VisitDeclRefExpr(clang::DeclRefExpr* DRE)
    {
        if (DRE->getDecl() == targetParam) {
            foundUsages.push_back(DRE);
        }
        return true;
    }
};

class VarCollector : public clang::RecursiveASTVisitor<VarCollector> {
public:
    std::set<std::string>& vars;
    explicit VarCollector(std::set<std::string>& v) : vars(v) {}

    bool VisitDeclRefExpr(clang::DeclRefExpr* expr)
    {
        if (auto* var = llvm::dyn_cast<clang::VarDecl>(expr->getDecl())) {
            vars.insert(var->getNameAsString());
        }
        return true;
    }
};

class VarExtractor : public clang::RecursiveASTVisitor<VarExtractor> {
public:
    std::set<std::string>& vars;
    explicit VarExtractor(std::set<std::string>& v) : vars(v) {}

    bool VisitDeclRefExpr(clang::DeclRefExpr* ref)
    {
        if (auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
            vars.insert(var->getNameAsString());
        }
        return true;
    }
};

} // namespace cpg

#endif // CPG_ANNOTATION_V2_H