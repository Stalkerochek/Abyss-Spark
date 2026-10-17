// Copyright NodeSmith 2026

#include "BlueprintAnalyzer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EventNodeInterface.h"
#include "K2Node_InputKey.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKeyEvent.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputTouch.h"
#include "K2Node_InputTouchEvent.h"
#include "K2Node_InputVectorAxisEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Knot.h"
#include "K2Node_Copy.h"
#include "K2Node_Message.h"
#include "K2Node_Composite.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

// ============================================================
// 入口
// ============================================================

FBPDocument FBlueprintAnalyzer::Analyze(UBlueprint* InBlueprint)
{
	FBlueprintAnalyzer Analyzer(InBlueprint);
	Analyzer.Doc.BlueprintName = InBlueprint->GetName();
	Analyzer.CollectVariables();
	Analyzer.CollectComponents();
	Analyzer.ProcessEvents();
	Analyzer.ProcessFunctions();
	Analyzer.CollectDependencies();
	return MoveTemp(Analyzer.Doc);
}

FBlueprintAnalyzer::FBlueprintAnalyzer(UBlueprint* InBlueprint)
	: Blueprint(InBlueprint)
{
}

// ============================================================
// 元信息收集
// ============================================================

void FBlueprintAnalyzer::CollectVariables()
{
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FBPVariableDoc VarDoc;
		VarDoc.Name = Var.VarName.ToString();
		VarDoc.Type = UEdGraphSchema_K2::TypeToText(Var.VarType).ToString();
		
		// 只在有有效默认值时记录，不猜测
		const FString& Def = Var.DefaultValue;
		if (!Def.IsEmpty() && Def != TEXT("None") && Def != TEXT("()"))
		{
			VarDoc.DefaultValue = Def;
		}
		
		Doc.Variables.Add(MoveTemp(VarDoc));
	}
}

void FBlueprintAnalyzer::CollectComponents()
{
	if (!Blueprint->SimpleConstructionScript)
	{
		return;
	}
	
	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->ComponentTemplate)
		{
			continue;
		}
		
		FBPComponentDoc CompDoc;
		CompDoc.Name = Node->GetVariableName().ToString();
		CompDoc.Type = Node->ComponentTemplate->GetClass()->GetName();
		Doc.Components.Add(MoveTemp(CompDoc));
	}
}

void FBlueprintAnalyzer::CollectDependencies()
{
	TSet<FString> DepSet;
	
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			// Cast 目标类型
			if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				if (CastNode->TargetType)
				{
					FString TypeName = CastNode->TargetType->GetName();
					TypeName.RemoveFromEnd(TEXT("_C"));
					
					// 只记录项目内蓝图类
					const FString PackageName = CastNode->TargetType->GetOutermost()->GetName();
					if (PackageName.StartsWith(TEXT("/Game/")))
					{
						DepSet.Add(TypeName);
					}
				}
			}
		}
	}
	
	// 变量类型中的项目内蓝图类
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Object && Var.VarType.PinSubCategoryObject.IsValid())
		{
			UObject* TypeObj = Var.VarType.PinSubCategoryObject.Get();
			if (TypeObj)
			{
				const FString PackageName = TypeObj->GetOutermost()->GetName();
				if (PackageName.StartsWith(TEXT("/Game/")))
				{
					FString TypeName = TypeObj->GetName();
					TypeName.RemoveFromEnd(TEXT("_C"));
					DepSet.Add(TypeName);
				}
			}
		}
	}
	
	TArray<FString> DepArray = DepSet.Array();
	DepArray.Sort();
	for (const FString& Dep : DepArray)
	{
		Doc.Dependencies.Add(Dep);
	}
}

// ============================================================
// 入口点处理
// ============================================================

void FBlueprintAnalyzer::ProcessEvents()
{
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		// 记录本页已被事件执行流覆盖的节点，供兜底扫描使用
		TSet<const UEdGraphNode*> AllVisited;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// 识别所有事件节点：
			// 1) UK2Node_Event 及其子类（CustomEvent / ComponentBoundEvent）
			// 2) 实现 IK2Node_EventNodeInterface 的输入节点（InputKey / InputAction / InputKeyEvent 等），
			//    它们不是 UK2Node_Event 的子类，但同样是事件入口
			const bool bIsEventNode =
				Cast<UK2Node_Event>(Node) != nullptr ||
				Node->GetClass()->ImplementsInterface(UK2Node_EventNodeInterface::StaticClass());

			if (!bIsEventNode)
			{
				continue;
			}

			FBPFunctionDoc EventDoc;

			// 事件名：取节点标题并去掉 "Event " 前缀
			FString EventName = CleanNodeTitle(Node);
			if (EventName.StartsWith(TEXT("Event ")))
			{
				EventName = EventName.RightChop(6);
			}
			EventDoc.Name = EventName;

			// 重置每个事件的临时变量命名空间
			EmittedResultNames.Empty();
			UsedNames.Empty();
			TempCounter = 0;

			TSet<const UEdGraphNode*> Visited;
			Visited.Add(Node);

			// 遍历该事件的所有执行输出（兼容 Then / Pressed / Released 等多种 Pin）
			WalkAllExecOutputs(Node, EventDoc.Statements, Visited);

			AllVisited.Append(Visited);

			Doc.Events.Add(MoveTemp(EventDoc));
		}

		// 兜底：折叠节点外部可能没有任何连接引脚，无法从事件执行流到达；
		// 只要图表中还存在未被覆盖的折叠节点/宏实例，就强制进入读取其内部全部节点
		EmittedResultNames.Empty();
		UsedNames.Empty();
		TempCounter = 0;

		FBPFunctionDoc OrphanDoc;
		OrphanDoc.Name = TEXT("UnconnectedGraphs");
		CollectUnreachedSubGraphs(Graph, OrphanDoc.Statements, AllVisited);
		if (OrphanDoc.Statements.Num() > 0)
		{
			Doc.Events.Add(MoveTemp(OrphanDoc));
		}
	}
}

void FBlueprintAnalyzer::ProcessFunctions()
{
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		
		FBPFunctionDoc FuncDoc;
		FuncDoc.Name = Graph->GetName();
		
		EmittedResultNames.Empty();
		UsedNames.Empty();
		TempCounter = 0;
		
		TSet<const UEdGraphNode*> Visited;
		
		// 找到函数入口节点
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (!EntryNode)
			{
				continue;
			}

			Visited.Add(EntryNode);
			WalkExec(GetThenPin(EntryNode), FuncDoc.Statements, Visited);
			break;
		}

		// 兜底：函数图表中未被入口执行流覆盖的折叠节点/宏实例也强制读取
		CollectUnreachedSubGraphs(Graph, FuncDoc.Statements, Visited);

		Doc.Functions.Add(MoveTemp(FuncDoc));
	}
}

// ============================================================
// Execution Flow
// ============================================================

void FBlueprintAnalyzer::WalkExec(UEdGraphPin* ExecPin, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	UEdGraphPin* Current = ExecPin;
	int32 Guard = 0;
	
	while (Current && Current->LinkedTo.Num() > 0 && Guard++ < 10000)
	{
		UEdGraphPin* NextPin = Current->LinkedTo[0];
		if (!NextPin)
		{
			break;
		}
		
		UEdGraphNode* Node = NextPin->GetOwningNode();
		if (!Node || Visited.Contains(Node))
		{
			break;
		}
		Visited.Add(Node);
		
		Current = HandleNode(Node, Out, Visited);
	}
}

UEdGraphPin* FBlueprintAnalyzer::HandleNode(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	// 路由节点：直接穿透
	if (Cast<UK2Node_Knot>(Node))
	{
		return GetFirstLinkedExecOutputPin(Node);
	}
	
	if (Cast<UK2Node_IfThenElse>(Node))
	{
		HandleIfThenElse(Node, Out, Visited);
		return nullptr;
	}
	
	if (Cast<UK2Node_ExecutionSequence>(Node))
	{
		HandleSequence(Node, Out, Visited);
		return nullptr;
	}
	
	// 折叠节点：递归进入其 BoundGraph
	// 注意：UK2Node_Composite 继承自 UK2Node_Tunnel，必须在 Tunnel 穿透检查之前处理
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		HandleComposite(CompositeNode, Out, Visited);
		// 折叠节点的输出执行 Pin 不一定叫 "Then"（可能有多个/自定义名），
		// 只认 Then 会导致后续整条链路丢失
		return ContinueOrWalkAllOutputs(Node, Out, Visited);
	}

	// 宏实例：递归进入宏图表
	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		HandleMacroInstance(MacroNode, Out, Visited);
		return ContinueOrWalkAllOutputs(Node, Out, Visited);
	}

	// 隧道节点（折叠图表/宏的入口出口）：直接穿透
	if (Cast<UK2Node_Tunnel>(Node))
	{
		return GetFirstLinkedExecOutputPin(Node);
	}

	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		HandleDynamicCast(CastNode, Out, Visited);
		// 非纯 Cast 的成功分支 Pin 名为 CastSucceeded（不是 Then）
		UEdGraphPin* ValidPin = Node->FindPin(UEdGraphSchema_K2::PN_CastSucceeded);
		return ValidPin ? ValidPin : GetThenPin(Node);
	}

	if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
	{
		HandleVariableSet(SetNode, Out, Visited);
		return GetThenPin(Node);
	}

	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		HandleCallFunction(CallNode, Out, Visited);
		// ExpandEnumAsExecs 等节点可能有多个执行输出：全部遍历，避免丢失分支
		return ContinueOrWalkAllOutputs(Node, Out, Visited);
	}

	// 事件分发调用（UK2Node_Message，如调用事件分发器）：渲染为 Dispatch 调用
	if (UK2Node_Message* MessageNode = Cast<UK2Node_Message>(Node))
	{
		HandleMessage(MessageNode, Out, Visited);
		return ContinueOrWalkAllOutputs(Node, Out, Visited);
	}

	// 兜底：输出节点标题作为原始语句
	const FString Title = CleanNodeTitle(Node);
	if (!Title.IsEmpty())
	{
		Out.Add(FBPStatement::MakeRaw(Title));
	}
	// 多执行输出的未知节点（如 Switch）：全部遍历，避免丢失分支
	return ContinueOrWalkAllOutputs(Node, Out, Visited);
}

void FBlueprintAnalyzer::HandleIfThenElse(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(Node);
	if (!BranchNode)
	{
		return;
	}
	
	// 解析条件表达式（可能产生前置语句）
	TArray<TSharedPtr<FBPStatement>> Preamble;
	TSharedPtr<FBPExpression> Condition = ResolveExpression(BranchNode->GetConditionPin(), Preamble);
	Out.Append(Preamble);
	
	TSharedPtr<FBPStatement> IfStmt = FBPStatement::MakeIf(Condition);
	
	// True 分支
	UEdGraphPin* ThenPin = GetThenPin(Node);
	if (ThenPin && ThenPin->LinkedTo.Num() > 0)
	{
		WalkExec(ThenPin, IfStmt->Body, Visited);
	}
	
	// False 分支
	UEdGraphPin* ElsePin = BranchNode->GetElsePin();
	if (ElsePin && ElsePin->LinkedTo.Num() > 0)
	{
		WalkExec(ElsePin, IfStmt->ElseBody, Visited);
	}
	
	Out.Add(IfStmt);
}

void FBlueprintAnalyzer::HandleSequence(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	TSharedPtr<FBPStatement> SeqStmt = MakeShared<FBPStatement>();
	SeqStmt->Type = EBPStmtType::Sequence;
	
	// 收集所有执行输出 Pin（Then_0, Then_1, ...），按名称排序保证顺序
	TArray<UEdGraphPin*> ExecPins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			ExecPins.Add(Pin);
		}
	}
	ExecPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
	{
		// "Then_10" 按字典序会排在 "Then_2" 之前，必须提取尾部数字做数值比较
		auto TrailingNumber = [](const UEdGraphPin& Pin, int32& OutNum) -> bool
		{
			const FString Name = Pin.PinName.ToString();
			int32 DigitStart = INDEX_NONE;
			for (int32 i = Name.Len() - 1; i >= 0; --i)
			{
				const TCHAR C = Name[i];
				if (C >= TEXT('0') && C <= TEXT('9'))
				{
					DigitStart = i;
				}
				else
				{
					break;
				}
			}
			if (DigitStart == INDEX_NONE || DigitStart == 0)
			{
				OutNum = 0;
				return false;
			}
			OutNum = FCString::Atoi(*Name.RightChop(DigitStart));
			return true;
		};
		int32 NumA = 0, NumB = 0;
		const bool bHasA = TrailingNumber(A, NumA);
		const bool bHasB = TrailingNumber(B, NumB);
		if (bHasA && bHasB)
		{
			return NumA < NumB;
		}
		return A.PinName.LexicalLess(B.PinName);
	});
	
	for (UEdGraphPin* Pin : ExecPins)
	{
		if (!Pin || Pin->LinkedTo.Num() == 0)
		{
			continue;
		}
		
		TArray<TSharedPtr<FBPStatement>> BranchBody;
		WalkExec(Pin, BranchBody, Visited);
		if (BranchBody.Num() > 0)
		{
			SeqStmt->Branches.Add(MoveTemp(BranchBody));
		}
	}
	
	if (SeqStmt->Branches.Num() > 0)
	{
		Out.Add(SeqStmt);
	}
}

void FBlueprintAnalyzer::HandleComposite(UK2Node_Composite* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	UEdGraph* BoundGraph = Node->BoundGraph;
	if (!BoundGraph)
	{
		// 无法进入折叠节点内部，保留节点信息避免丢失逻辑
		Out.Add(FBPStatement::MakeRaw(FString::Printf(TEXT("UnknownNode(Type = \"CollapsedGraph\", Name = \"%s\")"), *CleanNodeTitle(Node))));
		return;
	}
	WalkSubGraph(BoundGraph, Out, Visited);
}

void FBlueprintAnalyzer::HandleMacroInstance(UK2Node_MacroInstance* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	UEdGraph* MacroGraph = Node->GetMacroGraph();
	if (!MacroGraph)
	{
		// 无法进入宏内部，保留节点信息避免丢失逻辑
		Out.Add(FBPStatement::MakeRaw(FString::Printf(TEXT("UnknownNode(Type = \"Macro\", Name = \"%s\")"), *CleanNodeTitle(Node))));
		return;
	}
	WalkSubGraph(MacroGraph, Out, Visited);
}

void FBlueprintAnalyzer::WalkSubGraph(UEdGraph* SubGraph, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	if (!SubGraph)
	{
		return;
	}

	// 全量扫描策略（确保折叠节点内部逻辑不丢失）：
	// 1) 第一遍：优先从入口隧道（有执行输出连线的 Tunnel）开始遍历，保证主流程顺序正确
	// 2) 第二遍：扫描子图表中所有仍未访问的节点并强制处理（纯数据节点除外），
	//    即使节点完全没有执行引脚连线也会被读取，兜底捕获孤立/未连线的逻辑
	// 嵌套的折叠节点/宏会在 HandleNode → HandleComposite/HandleMacroInstance 中递归处理

	// 第一遍：入口隧道
	for (UEdGraphNode* Node : SubGraph->Nodes)
	{
		UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
		if (!Tunnel || Visited.Contains(Tunnel))
		{
			continue;
		}
		// Composite 继承自 Tunnel：嵌套折叠节点必须走第二遍的 HandleNode → HandleComposite
		// 递归进入内部，不能当作普通隧道穿透
		if (Cast<UK2Node_Composite>(Tunnel))
		{
			continue;
		}
		if (!GetFirstLinkedExecOutputPin(Tunnel))
		{
			continue;
		}
		Visited.Add(Tunnel);
		WalkAllExecOutputs(Tunnel, Out, Visited);
	}

	// 第二遍：兜底全量扫描（用 HandleNode 分发，保证嵌套折叠节点也能递归进入）。
	// Nodes 数组顺序 ≈ 节点创建顺序（近乎随机），必须按视觉位置排序（上→下、左→右），
	// 否则断链后补漏的语句顺序会和图表中的逻辑顺序完全不一致
	TArray<UEdGraphNode*> Remaining;
	for (UEdGraphNode* Node : SubGraph->Nodes)
	{
		if (!Node || Visited.Contains(Node))
		{
			continue;
		}

		// 纯数据节点（变量读取/纯函数/路由节点）没有执行语义，
		// 它们只在被数据流引用时有意义，跳过以免产生噪音
		const UK2Node* K2Node = Cast<UK2Node>(Node);
		if (Cast<UK2Node_Knot>(Node) || (K2Node && K2Node->IsNodePure()))
		{
			continue;
		}

		// 注释框只是视觉标注，没有执行语义
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			continue;
		}
		Remaining.Add(Node);
	}
	Remaining.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		if (A.NodePosY != B.NodePosY)
		{
			return A.NodePosY < B.NodePosY;
		}
		return A.NodePosX < B.NodePosX;
	});

	for (UEdGraphNode* Node : Remaining)
	{
		// Remaining 是提前快照：处理前面的节点时，执行链可能已经顺路访问过本节点
		//（例如链头节点一路走完了 IF → SetVectorParameterValue），必须复查避免重复输出
		if (Visited.Contains(Node))
		{
			continue;
		}
		Visited.Add(Node);
		UEdGraphPin* NextPin = HandleNode(Node, Out, Visited);
		WalkExec(NextPin, Out, Visited);
	}
}

void FBlueprintAnalyzer::CollectUnreachedSubGraphs(UEdGraph* Graph, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	if (!Graph)
	{
		return;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Visited.Contains(Node))
		{
			continue;
		}

		UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
		UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node);
		if (!Composite && !Macro)
		{
			continue;
		}

		// 该折叠节点/宏实例未被任何执行流覆盖（外部可能完全没有连接引脚），
		// 强制进入其内部图表读取全部节点；嵌套折叠节点由 WalkSubGraph 递归处理
		Visited.Add(Node);

		Out.Add(FBPStatement::MakeRaw(FString::Printf(TEXT("[CollapsedGraph: %s]"), *CleanNodeTitle(Node))));

		if (Composite)
		{
			HandleComposite(Composite, Out, Visited);
		}
		else
		{
			HandleMacroInstance(Macro, Out, Visited);
		}
	}
}

void FBlueprintAnalyzer::WalkAllExecOutputs(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	if (!Node)
	{
		return;
	}
	
	// 收集所有有连线的执行输出 Pin
	TArray<UEdGraphPin*> LinkedExecPins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->LinkedTo.Num() > 0)
		{
			LinkedExecPins.Add(Pin);
		}
	}
	
	if (LinkedExecPins.Num() == 0)
	{
		return;
	}
	
	// 单个输出：直接遍历（普通事件的 Then Pin）
	if (LinkedExecPins.Num() == 1)
	{
		WalkExec(LinkedExecPins[0], Out, Visited);
		return;
	}
	
	// 多个输出（如 InputKey 的 Pressed / Released）：为每个分支加 Pin 名标签
	for (UEdGraphPin* Pin : LinkedExecPins)
	{
		Out.Add(FBPStatement::MakeRaw(FString::Printf(TEXT("[%s]:"), *Pin->PinName.ToString())));
		WalkExec(Pin, Out, Visited);
	}
}

void FBlueprintAnalyzer::HandleDynamicCast(UK2Node_DynamicCast* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	// 目标类型名（去掉 _C 后缀）
	FString TypeName = TEXT("?");
	if (Node->TargetType)
	{
		TypeName = Node->TargetType->GetName();
		TypeName.RemoveFromEnd(TEXT("_C"));
	}
	
	// 解析 Cast 来源
	TArray<TSharedPtr<FBPStatement>> Preamble;
	TSharedPtr<FBPExpression> Source = ResolveExpression(Node->GetCastSourcePin(), Preamble);
	Out.Append(Preamble);
	
	// 为 Cast 结果注册变量名（基于类型名）
	const FString VarName = RegisterResultName(Node, NameFromTypeName(TypeName));
	
	// 输出: Player = Cast(Player, BP_Player)
	Out.Add(FBPStatement::MakeAssignment(VarName, FBPExpression::MakeCast(Source, TypeName)));
	
	// Cast Failed 分支（纯 Cast 节点没有该 Pin，引擎的 GetInvalidCastPin 会断言，故手动查找）
	UEdGraphPin* FailedPin = Node->FindPin(UEdGraphSchema_K2::PN_CastFailed);
	if (FailedPin && FailedPin->LinkedTo.Num() > 0)
	{
		TSharedPtr<FBPStatement> IfFailed = FBPStatement::MakeIf(
			FBPExpression::MakeVarRef(TEXT("CastFailed")));
		WalkExec(FailedPin, IfFailed->Body, Visited);
		Out.Add(IfFailed);
	}
}

void FBlueprintAnalyzer::HandleVariableSet(UK2Node_VariableSet* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	const FString VarName = Node->GetVarNameString();
	
	// 找到值输入 Pin（与变量同名的输入 Pin）
	UEdGraphPin* ValuePin = nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input &&
			Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
			Pin->PinName.ToString() == VarName)
		{
			ValuePin = Pin;
			break;
		}
	}
	
	TArray<TSharedPtr<FBPStatement>> Preamble;
	TSharedPtr<FBPExpression> ValueExpr;
	if (ValuePin)
	{
		ValueExpr = ResolveExpression(ValuePin, Preamble);
	}
	else
	{
		ValueExpr = FBPExpression::MakeLiteral(TEXT("?"));
	}
	Out.Append(Preamble);
	
	Out.Add(FBPStatement::MakeAssignment(VarName, ValueExpr));
}

void FBlueprintAnalyzer::HandleCallFunction(UK2Node_CallFunction* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	// 防重复：若该节点已被数据流提升为赋值语句，则跳过执行流侧的重复输出
	if (EmittedResultNames.Contains(Node))
	{
		return;
	}
	
	const FString FuncName = Node->GetFunctionName().ToString();
	
	TArray<TSharedPtr<FBPStatement>> Preamble;
	
	// 收集参数（排除 exec 和 self），并过滤隐式 None 参数
	TArray<TSharedPtr<FBPExpression>> Args;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			continue;
		}
		
		TSharedPtr<FBPExpression> ArgExpr = ResolveExpression(Pin, Preamble);
		// 过滤"未连接 Pin 的默认 None"，避免污染文档
		if (!IsImplicitNoneArg(Pin, ArgExpr))
		{
			Args.Add(ArgExpr);
		}
	}
	Out.Append(Preamble);
	
	// 特殊处理: Delay → WAIT
	if (FuncName == TEXT("Delay"))
	{
		FString Duration = TEXT("?");
		if (Args.Num() > 0 && Args[0].IsValid())
		{
			Duration = Args[0]->Text.IsEmpty() ? TEXT("?") : Args[0]->Text;
		}
		Out.Add(FBPStatement::MakeRaw(FString::Printf(TEXT("WAIT %s seconds"), *Duration)));
		return;
	}
	
	// 获取网络复制标记（Server/Client/Multicast）
	const FString RepTag = GetReplicationTag(Node);
	
	TSharedPtr<FBPExpression> CallExpr = FBPExpression::MakeFuncCall(FuncName, MoveTemp(Args));
	
	// 如果返回值被使用，生成赋值语句；否则生成普通调用
	UEdGraphPin* ReturnPin = Node->GetReturnValuePin();
	TSharedPtr<FBPStatement> Stmt;
	if (ReturnPin && ReturnPin->LinkedTo.Num() > 0)
	{
		const FString VarName = RegisterResultName(Node, FString());
		Stmt = FBPStatement::MakeAssignment(VarName, CallExpr);
	}
	else
	{
		Stmt = FBPStatement::MakeFuncCall(CallExpr);
	}
	
	// 附加 RPC 前缀，保留网络执行信息
	if (!RepTag.IsEmpty())
	{
		Stmt->Prefix = FString::Printf(TEXT("[%s] "), *RepTag);
	}
	Out.Add(Stmt);
}

void FBlueprintAnalyzer::HandleMessage(UK2Node_Message* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	if (!Node)
	{
		return;
	}

	TArray<TSharedPtr<FBPStatement>> Preamble;

	// 收集参数（排除 exec 和 self），并过滤隐式 None 参数
	TArray<TSharedPtr<FBPExpression>> Args;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			continue;
		}

		TSharedPtr<FBPExpression> ArgExpr = ResolveExpression(Pin, Preamble);
		if (!IsImplicitNoneArg(Pin, ArgExpr))
		{
			Args.Add(ArgExpr);
		}
	}
	Out.Append(Preamble);

	// 事件分发调用：以 Dispatch 调用语句输出，保留分发语义
	TSharedPtr<FBPStatement> Stmt = FBPStatement::MakeFuncCall(
		FBPExpression::MakeFuncCall(CleanNodeTitle(Node), MoveTemp(Args)));
	Stmt->Prefix = TEXT("[Dispatch] ");
	Out.Add(Stmt);
}

UEdGraphPin* FBlueprintAnalyzer::ContinueOrWalkAllOutputs(UEdGraphNode* Node, TArray<TSharedPtr<FBPStatement>>& Out, TSet<const UEdGraphNode*>& Visited)
{
	if (!Node)
	{
		return nullptr;
	}

	// 统计已连线的执行输出 Pin
	int32 LinkedCount = 0;
	UEdGraphPin* Single = nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->LinkedTo.Num() > 0)
		{
			++LinkedCount;
			Single = Pin;
		}
	}

	// 0 个 → 返回 nullptr 结束链；1 个 → 返回该 Pin 继续链式遍历
	if (LinkedCount <= 1)
	{
		return Single;
	}

	// 多个输出（如折叠节点的多个出口、Switch 的多分支）：全部遍历，流程已自行消费
	WalkAllExecOutputs(Node, Out, Visited);
	return nullptr;
}

// ============================================================
// Data Flow
// ============================================================

TSharedPtr<FBPExpression> FBlueprintAnalyzer::ResolveExpression(UEdGraphPin* InputPin, TArray<TSharedPtr<FBPStatement>>& OutPreamble, int32 Depth)
{
	if (!InputPin || Depth > MaxResolveDepth)
	{
		return FBPExpression::MakeLiteral(TEXT("?"));
	}
	
	// 未连线：读取默认值
	if (InputPin->LinkedTo.Num() == 0)
	{
		return MakePinDefaultLiteral(InputPin);
	}
	
	UEdGraphPin* SourcePin = InputPin->LinkedTo[0];
	if (!SourcePin)
	{
		return FBPExpression::MakeLiteral(TEXT("?"));
	}
	
	UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
	if (!SourceNode)
	{
		return FBPExpression::MakeLiteral(TEXT("?"));
	}
	
	// 已输出过的节点：直接引用其变量名
	if (const FString* ExistingName = EmittedResultNames.Find(SourceNode))
	{
		return FBPExpression::MakeVarRef(*ExistingName);
	}
	
	// 路由节点：穿透
	if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(SourceNode))
	{
		UEdGraphPin* KnotInput = nullptr;
		for (UEdGraphPin* Pin : Knot->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				KnotInput = Pin;
				break;
			}
		}
		return ResolveExpression(KnotInput, OutPreamble, Depth + 1);
	}
	
	// Copy（复制）节点：纯实现细节，直接穿透为其输入源
	if (UK2Node_Copy* CopyNode = Cast<UK2Node_Copy>(SourceNode))
	{
		return ResolveExpression(CopyNode->GetInputReferencePin(), OutPreamble, Depth + 1);
	}
	
	// Set 节点的输出 Pin（链式输出）：引用被设置的变量本身
	if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(SourceNode))
	{
		return FBPExpression::MakeVarRef(SetNode->GetVarNameString());
	}
	
	// 事件/函数入口的输出 Pin（事件参数/函数参数）：直接以 Pin 名引用
	if (Cast<UK2Node_Event>(SourceNode) || Cast<UK2Node_CustomEvent>(SourceNode) || Cast<UK2Node_FunctionEntry>(SourceNode))
	{
		return FBPExpression::MakeVarRef(SourcePin->PinName.ToString());
	}
	
	// 变量 Get
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))
	{
		FString Owner;
		
		// 检查 self Pin 是否连线（访问其他对象的变量）
		for (UEdGraphPin* Pin : VarGet->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == UEdGraphSchema_K2::PN_Self && Pin->LinkedTo.Num() > 0)
			{
				TSharedPtr<FBPExpression> OwnerExpr = ResolveExpression(Pin, OutPreamble, Depth + 1);
				if (OwnerExpr.IsValid() && OwnerExpr->Type == EBPExprType::VariableRef)
				{
					Owner = OwnerExpr->Owner.IsEmpty() ? OwnerExpr->Text : FString::Printf(TEXT("%s.%s"), *OwnerExpr->Owner, *OwnerExpr->Text);
				}
				break;
			}
		}
		
		return FBPExpression::MakeVarRef(VarGet->GetVarNameString(), Owner);
	}
	
	// 纯 Cast：作为表达式内联
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(SourceNode))
	{
		FString TypeName = TEXT("?");
		if (CastNode->TargetType)
		{
			TypeName = CastNode->TargetType->GetName();
			TypeName.RemoveFromEnd(TEXT("_C"));
		}
		TSharedPtr<FBPExpression> Source = ResolveExpression(CastNode->GetCastSourcePin(), OutPreamble, Depth + 1);
		return FBPExpression::MakeCast(Source, TypeName);
	}
	
	// 函数调用节点
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(SourceNode))
	{
		const FString FuncName = CallNode->GetFunctionName().ToString();
		
		// 纯类型转换节点（Conv_*）：透传源表达式，隐藏实现细节
		if (FuncName.StartsWith(TEXT("Conv_")))
		{
			TSharedPtr<FBPExpression> Simplified = TrySimplifyConversion(CallNode, OutPreamble, Depth);
			if (Simplified.IsValid())
			{
				return Simplified;
			}
		}
		
		// 收集输入参数
		TArray<UEdGraphPin*> InputPins;
		UEdGraphPin* SelfPin = nullptr;
		for (UEdGraphPin* Pin : CallNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
			{
				SelfPin = Pin;
				continue;
			}
			InputPins.Add(Pin);
		}
		
		// Boolean NOT 一元运算：NOT A
		if (FuncName == TEXT("Not_PreBool") || FuncName == TEXT("Not_Bool"))
		{
			if (InputPins.Num() >= 1)
			{
				TSharedPtr<FBPExpression> Operand = ResolveExpression(InputPins[0], OutPreamble, Depth + 1);
				TSharedPtr<FBPExpression> NotExpr = FBPExpression::MakeBinaryOp(TEXT("NOT"), nullptr, Operand);
				return NotExpr;
			}
		}
		
		// 运算符节点 → 二元表达式
		const FString OpSymbol = GetOperatorSymbol(FuncName);
		if (!OpSymbol.IsEmpty() && InputPins.Num() >= 2)
		{
			TSharedPtr<FBPExpression> Left = ResolveExpression(InputPins[0], OutPreamble, Depth + 1);
			TSharedPtr<FBPExpression> Right = ResolveExpression(InputPins[1], OutPreamble, Depth + 1);
			return FBPExpression::MakeBinaryOp(OpSymbol, Left, Right);
		}
		
		// 容器操作语义化
		if (Cast<UK2Node_CallArrayFunction>(CallNode))
		{
			// Array_Get → Array[Index]
			if (FuncName == TEXT("Array_Get"))
			{
				TSharedPtr<FBPExpression> ArrayExpr;
				TSharedPtr<FBPExpression> IndexExpr;
				for (UEdGraphPin* Pin : InputPins)
				{
					TSharedPtr<FBPExpression> ArgExpr = ResolveExpression(Pin, OutPreamble, Depth + 1);
					if (!ArrayExpr.IsValid())
					{
						ArrayExpr = ArgExpr;
					}
					else if (!IndexExpr.IsValid())
					{
						IndexExpr = ArgExpr;
					}
				}
				if (ArrayExpr.IsValid() && IndexExpr.IsValid())
				{
					return FBPExpression::MakeArrayAccess(ArrayExpr, IndexExpr);
				}
			}
			
			// Array_IsValidIndex → IsValidIndex(Array, Index)
			if (FuncName == TEXT("Array_IsValidIndex"))
			{
				TArray<TSharedPtr<FBPExpression>> Args;
				for (UEdGraphPin* Pin : InputPins)
				{
					Args.Add(ResolveExpression(Pin, OutPreamble, Depth + 1));
				}
				return FBPExpression::MakeFuncCall(TEXT("IsValidIndex"), MoveTemp(Args));
			}
			
			// Array_Length → Array.Length
			if (FuncName == TEXT("Array_Length") && InputPins.Num() >= 1)
			{
				TSharedPtr<FBPExpression> ArrayExpr = ResolveExpression(InputPins[0], OutPreamble, Depth + 1);
				TSharedPtr<FBPExpression> LengthExpr = FBPExpression::MakeVarRef(TEXT("Length"));
				if (ArrayExpr.IsValid() && ArrayExpr->Type == EBPExprType::VariableRef)
				{
					LengthExpr->Owner = ArrayExpr->Owner.IsEmpty() ? ArrayExpr->Text : FString::Printf(TEXT("%s.%s"), *ArrayExpr->Owner, *ArrayExpr->Text);
				}
				return LengthExpr;
			}
		}
		
		// Map_Find → Map[Key]
		if (FuncName == TEXT("Map_Find") && InputPins.Num() >= 2)
		{
			TSharedPtr<FBPExpression> MapExpr = ResolveExpression(InputPins[0], OutPreamble, Depth + 1);
			TSharedPtr<FBPExpression> KeyExpr = ResolveExpression(InputPins[1], OutPreamble, Depth + 1);
			return FBPExpression::MakeArrayAccess(MapExpr, KeyExpr);
		}
		
		// IsValid → Target != None
		if (FuncName == TEXT("IsValid") && InputPins.Num() >= 1)
		{
			TSharedPtr<FBPExpression> Target = ResolveExpression(InputPins[0], OutPreamble, Depth + 1);
			return FBPExpression::MakeBinaryOp(TEXT("!="), Target, FBPExpression::MakeNone());
		}
		
		// 纯函数 → 内联函数调用表达式
		if (CallNode->IsNodePure())
		{
			TArray<TSharedPtr<FBPExpression>> Args;
			for (UEdGraphPin* Pin : InputPins)
			{
				TSharedPtr<FBPExpression> ArgExpr = ResolveExpression(Pin, OutPreamble, Depth + 1);
				if (!IsImplicitNoneArg(Pin, ArgExpr))
				{
					Args.Add(ArgExpr);
				}
			}
			
			TSharedPtr<FBPExpression> CallExpr = FBPExpression::MakeFuncCall(FuncName, MoveTemp(Args));
			
			// 解析调用目标（self Pin 连线时）
			if (SelfPin && SelfPin->LinkedTo.Num() > 0)
			{
				TSharedPtr<FBPExpression> TargetExpr = ResolveExpression(SelfPin, OutPreamble, Depth + 1);
				if (TargetExpr.IsValid() && TargetExpr->Type == EBPExprType::VariableRef)
				{
					CallExpr->Owner = TargetExpr->Owner.IsEmpty() ? TargetExpr->Text : FString::Printf(TEXT("%s.%s"), *TargetExpr->Owner, *TargetExpr->Text);
				}
			}
			
			return CallExpr;
		}
		
		// 非纯函数的结果被当作数据使用 → 提升为赋值语句
		TArray<TSharedPtr<FBPExpression>> Args;
		for (UEdGraphPin* Pin : InputPins)
		{
			TSharedPtr<FBPExpression> ArgExpr = ResolveExpression(Pin, OutPreamble, Depth + 1);
			if (!IsImplicitNoneArg(Pin, ArgExpr))
			{
				Args.Add(ArgExpr);
			}
		}
		
		const FString VarName = RegisterResultName(SourceNode, FString());
		OutPreamble.Add(FBPStatement::MakeAssignment(VarName, FBPExpression::MakeFuncCall(FuncName, MoveTemp(Args))));
		return FBPExpression::MakeVarRef(VarName);
	}
	
	// 兜底：使用节点标题作为表达式
	const FString Title = CleanNodeTitle(SourceNode);
	return FBPExpression::MakeLiteral(Title.IsEmpty() ? TEXT("?") : Title);
}

// ============================================================
// 辅助函数
// ============================================================

FString FBlueprintAnalyzer::RegisterResultName(const UEdGraphNode* Node, const FString& SuggestedName)
{
	if (const FString* Existing = EmittedResultNames.Find(Node))
	{
		return *Existing;
	}
	
	FString BaseName = SuggestedName.IsEmpty() ? TEXT("Temp") : SuggestedName;
	FString FinalName = BaseName;
	int32 Suffix = 2;
	while (UsedNames.Contains(FinalName))
	{
		FinalName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
	}
	
	UsedNames.Add(FinalName);
	EmittedResultNames.Add(Node, FinalName);
	return FinalName;
}

FString FBlueprintAnalyzer::NameFromTypeName(const FString& TypeName)
{
	FString Name = TypeName;
	Name.RemoveFromEnd(TEXT("_C"));
	Name.RemoveFromStart(TEXT("BP_"));
	return Name.IsEmpty() ? TEXT("Temp") : Name;
}

UEdGraphPin* FBlueprintAnalyzer::GetThenPin(const UEdGraphNode* Node)
{
	return GetExecOutputPin(Node, UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* FBlueprintAnalyzer::GetExecOutputPin(const UEdGraphNode* Node, const FName& PinName)
{
	if (!Node)
	{
		return nullptr;
	}
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->PinName == PinName)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FBlueprintAnalyzer::GetFirstLinkedExecOutputPin(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}
	
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output &&
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
			Pin->LinkedTo.Num() > 0)
		{
			return Pin;
		}
	}
	return nullptr;
}

TSharedPtr<FBPExpression> FBlueprintAnalyzer::MakePinDefaultLiteral(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return FBPExpression::MakeLiteral(TEXT("?"));
	}
	
	const FName& Category = Pin->PinType.PinCategory;
	
	// 字符串/文本/名称：带引号
	if (Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text || Category == UEdGraphSchema_K2::PC_Name)
	{
		FString Value = Pin->DefaultValue;
		if (Value.IsEmpty() && !Pin->DefaultTextValue.IsEmpty())
		{
			Value = Pin->DefaultTextValue.ToString();
		}
		return FBPExpression::MakeLiteral(Value, true);
	}
	
	// 对象引用
	if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class)
	{
		if (Pin->DefaultObject)
		{
			return FBPExpression::MakeLiteral(Pin->DefaultObject->GetName());
		}
		return FBPExpression::MakeNone();
	}
	
	// 布尔
	if (Category == UEdGraphSchema_K2::PC_Boolean)
	{
		return FBPExpression::MakeLiteral(Pin->DefaultValue == TEXT("true") ? TEXT("true") : TEXT("false"));
	}
	
	// 数值等其他类型
	if (!Pin->DefaultValue.IsEmpty())
	{
		return FBPExpression::MakeLiteral(Pin->DefaultValue);
	}
	
	return FBPExpression::MakeLiteral(TEXT("0"));
}

TSharedPtr<FBPExpression> FBlueprintAnalyzer::TrySimplifyConversion(UK2Node_CallFunction* Node, TArray<TSharedPtr<FBPStatement>>& OutPreamble, int32 Depth)
{
	// Conv_* 节点：找到唯一的非 exec 输入 Pin，透传其源表达式
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		// 透传源表达式（隐藏纯类型转换的实现细节）
		return ResolveExpression(Pin, OutPreamble, Depth + 1);
	}
	return nullptr;
}

FString FBlueprintAnalyzer::GetReplicationTag(UK2Node_CallFunction* Node)
{
	if (!Node)
	{
		return FString();
	}
	
	UFunction* Func = Node->GetTargetFunction();
	if (!Func)
	{
		return FString();
	}
	
	if (Func->HasAnyFunctionFlags(FUNC_NetMulticast))
	{
		return TEXT("Multicast");
	}
	if (Func->HasAnyFunctionFlags(FUNC_NetServer))
	{
		return TEXT("Server");
	}
	if (Func->HasAnyFunctionFlags(FUNC_NetClient))
	{
		return TEXT("Client");
	}
	return FString();
}

bool FBlueprintAnalyzer::IsImplicitNoneArg(const UEdGraphPin* Pin, const TSharedPtr<FBPExpression>& Expr)
{
	// 只过滤"未连线且默认值为 None"的对象引用 Pin
	if (!Pin || Pin->LinkedTo.Num() > 0)
	{
		return false;
	}
	if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object)
	{
		return false;
	}
	return Expr.IsValid() && Expr->Type == EBPExprType::NoneRef;
}

FString FBlueprintAnalyzer::GetOperatorSymbol(const FString& FunctionName)
{
	// 去掉类型后缀: Add_FloatFloat → Add
	FString Base = FunctionName;
	int32 UnderscoreIdx;
	if (Base.FindChar(TEXT('_'), UnderscoreIdx))
	{
		Base = Base.Left(UnderscoreIdx);
	}
	
	if (Base == TEXT("Add")) return TEXT("+");
	if (Base == TEXT("Subtract")) return TEXT("-");
	if (Base == TEXT("Multiply")) return TEXT("*");
	if (Base == TEXT("Divide")) return TEXT("/");
	if (Base == TEXT("Greater")) return TEXT(">");
	if (Base == TEXT("GreaterEqual")) return TEXT(">=");
	if (Base == TEXT("Less")) return TEXT("<");
	if (Base == TEXT("LessEqual")) return TEXT("<=");
	if (Base == TEXT("Equal")) return TEXT("==");
	if (Base == TEXT("NotEqual")) return TEXT("!=");
	if (Base == TEXT("BooleanAND")) return TEXT("AND");
	if (Base == TEXT("BooleanOR")) return TEXT("OR");
	if (Base == TEXT("BooleanNAND")) return TEXT("NAND");
	if (Base == TEXT("Concatenate")) return TEXT("+");
	
	return FString();
}

FString FBlueprintAnalyzer::CleanNodeTitle(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}
	
	FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Title = Title.Replace(TEXT("\n"), TEXT(" "));
	Title = Title.Replace(TEXT("\r"), TEXT(""));
	return Title.TrimStartAndEnd();
}
