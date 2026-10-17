// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "BlueprintDocTypes.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node_CallFunction;
class UK2Node_DynamicCast;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_Composite;
class UK2Node_MacroInstance;
class UK2Node_Message;

/**
 * Blueprint 分析器：
 * 分析 Execution Flow 和 Data Flow，将蓝图图表转换为结构化 IR（FBPDocument）
 */
class FBlueprintAnalyzer
{
public:
	/** 分析蓝图，生成文档 IR */
	static FBPDocument Analyze(UBlueprint* InBlueprint);

private:
	explicit FBlueprintAnalyzer(UBlueprint* InBlueprint);

	UBlueprint* Blueprint;
	FBPDocument Doc;

	/** 已在执行流中输出过结果的节点 → 分配的变量名（用于数据流引用） */
	TMap<const UEdGraphNode*, FString> EmittedResultNames;

	/** 已使用的变量名集合（避免命名冲突） */
	TSet<FString> UsedNames;

	/** 临时变量计数器 */
	int32 TempCounter = 0;

	// ---- 元信息收集 ----
	void CollectVariables();
	void CollectComponents();
	void CollectDependencies();

	// ---- 入口点处理 ----
	void ProcessEvents();
	void ProcessFunctions();

	// ---- Execution Flow ----
	/** 沿执行链遍历；HandleNode 返回下一个要继续的执行 Pin（返回 nullptr 表示节点内部已自行消费流程） */
	void WalkExec(UEdGraphPin* ExecPin, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	UEdGraphPin* HandleNode(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	void HandleIfThenElse(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	void HandleSequence(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	void HandleDynamicCast(UK2Node_DynamicCast* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	void HandleVariableSet(UK2Node_VariableSet* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);
	void HandleCallFunction(UK2Node_CallFunction* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 处理事件分发调用节点（UK2Node_Message）：渲染为 Dispatch 调用语句 */
	void HandleMessage(UK2Node_Message* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 节点执行输出的统一收尾：单个已连线输出时返回它以延续链式遍历；
	 *  多个输出时逐个遍历（自动加 Pin 名标签）并返回 nullptr（流程已自行消费）。
	 *  折叠节点/宏/分发调用的输出 Pin 不一定叫 "Then" 且可能有多个，必须走此逻辑 */
	UEdGraphPin* ContinueOrWalkAllOutputs(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 处理折叠节点（Collapsed Graph）：递归进入其 BoundGraph 的入口 */
	void HandleComposite(UK2Node_Composite* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 处理宏实例：递归进入宏图表的入口 */
	void HandleMacroInstance(UK2Node_MacroInstance* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 从一个子图表（折叠/宏）的入口节点开始遍历执行流；
	 *  采用"全量扫描"策略：进入子图表后强制处理所有未被访问的节点（纯数据节点除外），
	 *  确保折叠节点内部即使完全没有连线也不会丢失逻辑，并对嵌套折叠节点递归 */
	void WalkSubGraph(UEdGraph* SubGraph, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 兜底扫描：找出图表中未被任何执行流覆盖的折叠节点/宏实例并强制进入读取。
	 *  折叠节点外部可能没有任何连接引脚，无法从事件/函数入口到达，必须单独发现 */
	void CollectUnreachedSubGraphs(UEdGraph* Graph, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	/** 遍历一个事件节点的所有已连线执行输出 Pin（如 InputKey 的 Pressed/Released）。
	 *  单个输出时直接遍历；多个输出时为每个分支加 Pin 名标签 */
	void WalkAllExecOutputs(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited);

	// ---- Data Flow ----
	/** 从输入 Pin 反向解析数据表达式；Impure 中间节点会被提升为赋值语句放入 OutPreamble */
	TSharedPtr<FBPExpression> ResolveExpression(UEdGraphPin* InputPin, TArray<TSharedPtr<FBPStatement>>& OutPreamble, int32 Depth = 0);

	// ---- 语义清洗 ----
	/** 判断函数是否为纯类型转换（Conv_*），返回转换后的源表达式（透传） */
	TSharedPtr<FBPExpression> TrySimplifyConversion(UK2Node_CallFunction* Node, TArray<TSharedPtr<FBPStatement>>& OutPreamble, int32 Depth);

	/** 获取函数的网络复制标记：Server / Client / Multicast / 空 */
	static FString GetReplicationTag(UK2Node_CallFunction* Node);

	/** 判断参数表达式是否为"未连接 Pin 的默认 None"，应被过滤 */
	static bool IsImplicitNoneArg(const UEdGraphPin* Pin, const TSharedPtr<FBPExpression>& Expr);

	// ---- 辅助 ----
	/** 为节点的结果注册一个变量名 */
	FString RegisterResultName(const UEdGraphNode* Node, const FString& SuggestedName);

	/** 从类型名生成可读变量名（去掉 BP_ 前缀和 _C 后缀） */
	static FString NameFromTypeName(const FString& TypeName);

	/** 获取节点的 Then 执行输出 Pin */
	static UEdGraphPin* GetThenPin(const UEdGraphNode* Node);

	/** 获取节点指定名称的执行输出 Pin */
	static UEdGraphPin* GetExecOutputPin(const UEdGraphNode* Node, const FName& PinName);

	/** 获取节点第一个有连线的执行输出 Pin */
	static UEdGraphPin* GetFirstLinkedExecOutputPin(const UEdGraphNode* Node);

	/** 从未连线的 Pin 读取默认值字面量 */
	static TSharedPtr<FBPExpression> MakePinDefaultLiteral(const UEdGraphPin* Pin);

	/** 将函数名映射为运算符符号（如 Add_FloatFloat → +），返回空表示非运算符 */
	static FString GetOperatorSymbol(const FString& FunctionName);

	/** 清理节点标题文本 */
	static FString CleanNodeTitle(const UEdGraphNode* Node);

	/** 表达式解析最大深度 */
	static constexpr int32 MaxResolveDepth = 32;
};
