// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"

/**
 * Blueprint → 结构化伪代码文档的内部 IR（中间表示）
 * 仅插件内部使用，最终输出为纯文本伪代码
 */

// ============================================================
// Expression（表达式）
// ============================================================

enum class EBPExprType : uint8
{
	Literal,        // 字面量: 100, "text", true
	VariableRef,    // 变量引用: Health, Player.Health
	FuncCall,       // 函数调用表达式: GetPlayerCharacter()
	BinaryOp,       // 二元运算: A + B, A <= B（一元运算时 Left 为空，如 NOT A）
	CastExpr,       // Cast(Source, Type)
	PropertyAccess, // Object.Property
	ArrayAccess,    // Array[Index]
	SelfRef,        // Self
	NoneRef,        // None
};

struct FBPExpression
{
	EBPExprType Type = EBPExprType::Literal;
	FString Text;       // 字面量值 / 变量名 / 函数名 / 运算符
	FString Owner;      // VariableRef 的所属对象 / FuncCall 的调用目标
	FString SubType;    // CastExpr 的目标类型
	bool bQuoted = false; // 字面量是否为字符串（输出时加引号）
	TSharedPtr<FBPExpression> Left;
	TSharedPtr<FBPExpression> Right;
	TArray<TSharedPtr<FBPExpression>> Args;  // FuncCall 的参数列表

	FBPExpression() = default;

	static TSharedPtr<FBPExpression> MakeLiteral(const FString& InValue, bool bInQuoted = false)
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::Literal;
		E->Text = InValue;
		E->bQuoted = bInQuoted;
		return E;
	}

	static TSharedPtr<FBPExpression> MakeVarRef(const FString& InName, const FString& InOwner = FString())
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::VariableRef;
		E->Text = InName;
		E->Owner = InOwner;
		return E;
	}

	static TSharedPtr<FBPExpression> MakeFuncCall(const FString& InName, TArray<TSharedPtr<FBPExpression>> InArgs = {})
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::FuncCall;
		E->Text = InName;
		E->Args = MoveTemp(InArgs);
		return E;
	}

	static TSharedPtr<FBPExpression> MakeBinaryOp(const FString& InOp, TSharedPtr<FBPExpression> InLeft, TSharedPtr<FBPExpression> InRight)
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::BinaryOp;
		E->Text = InOp;
		E->Left = MoveTemp(InLeft);
		E->Right = MoveTemp(InRight);
		return E;
	}

	static TSharedPtr<FBPExpression> MakeCast(TSharedPtr<FBPExpression> InSource, const FString& InTargetType)
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::CastExpr;
		E->Left = MoveTemp(InSource);
		E->SubType = InTargetType;
		return E;
	}

	static TSharedPtr<FBPExpression> MakeArrayAccess(TSharedPtr<FBPExpression> InArray, TSharedPtr<FBPExpression> InIndex)
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::ArrayAccess;
		E->Left = MoveTemp(InArray);
		E->Right = MoveTemp(InIndex);
		return E;
	}

	static TSharedPtr<FBPExpression> MakeSelf()
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::SelfRef;
		E->Text = TEXT("Self");
		return E;
	}

	static TSharedPtr<FBPExpression> MakeNone()
	{
		TSharedPtr<FBPExpression> E = MakeShared<FBPExpression>();
		E->Type = EBPExprType::NoneRef;
		E->Text = TEXT("None");
		return E;
	}
};

// ============================================================
// Statement（语句）
// ============================================================

enum class EBPStmtType : uint8
{
	Assignment,     // X = Expr
	FuncCall,       // FuncName(args)
	IfStatement,    // IF cond: ... ELSE: ...
	Sequence,       // SEQUENCE: branch0, branch1...
	Raw,            // 兜底原始文本
};

struct FBPStatement
{
	EBPStmtType Type = EBPStmtType::Raw;
	FString Target;                  // 赋值目标名
	TSharedPtr<FBPExpression> Expr;  // 赋值 RHS / IF 条件 / 函数调用表达式
	TArray<TSharedPtr<FBPStatement>> Body;       // IF-true 体
	TArray<TSharedPtr<FBPStatement>> ElseBody;   // IF-false 体
	TArray<TArray<TSharedPtr<FBPStatement>>> Branches; // Sequence 各分支
	FString RawText;                 // Raw 类型的文本
	FString Prefix;                  // 语句前缀（如 RPC 标记 [Server]）

	FBPStatement() = default;

	static TSharedPtr<FBPStatement> MakeAssignment(const FString& InTarget, TSharedPtr<FBPExpression> InExpr)
	{
		TSharedPtr<FBPStatement> S = MakeShared<FBPStatement>();
		S->Type = EBPStmtType::Assignment;
		S->Target = InTarget;
		S->Expr = MoveTemp(InExpr);
		return S;
	}

	static TSharedPtr<FBPStatement> MakeFuncCall(TSharedPtr<FBPExpression> InCallExpr)
	{
		TSharedPtr<FBPStatement> S = MakeShared<FBPStatement>();
		S->Type = EBPStmtType::FuncCall;
		S->Expr = MoveTemp(InCallExpr);
		return S;
	}

	static TSharedPtr<FBPStatement> MakeIf(TSharedPtr<FBPExpression> InCondition)
	{
		TSharedPtr<FBPStatement> S = MakeShared<FBPStatement>();
		S->Type = EBPStmtType::IfStatement;
		S->Expr = MoveTemp(InCondition);
		return S;
	}

	static TSharedPtr<FBPStatement> MakeRaw(const FString& InText)
	{
		TSharedPtr<FBPStatement> S = MakeShared<FBPStatement>();
		S->Type = EBPStmtType::Raw;
		S->RawText = InText;
		return S;
	}
};

// ============================================================
// 文档级结构
// ============================================================

struct FBPFunctionDoc
{
	FString Name;
	TArray<TSharedPtr<FBPStatement>> Statements;
};

struct FBPVariableDoc
{
	FString Name;
	FString Type;
	FString DefaultValue;
};

struct FBPComponentDoc
{
	FString Name;
	FString Type;
};

struct FBPDocument
{
	FString BlueprintName;
	TArray<FBPVariableDoc> Variables;
	TArray<FBPComponentDoc> Components;
	TArray<FString> Dependencies;
	TArray<FBPFunctionDoc> Events;
	TArray<FBPFunctionDoc> Functions;
};
