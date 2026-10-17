// Copyright NodeSmith 2026

#include "BlueprintDocGenerator.h"

FString FBlueprintDocGenerator::Generate(const FBPDocument& Doc)
{
	TArray<FString> Lines;
	
	// 标题
	Lines.Add(FString::Printf(TEXT("# Blueprint: %s"), *Doc.BlueprintName));
	Lines.Add(TEXT(""));
	
	// Variables
	if (Doc.Variables.Num() > 0)
	{
		Lines.Add(TEXT("## Variables"));
		Lines.Add(TEXT(""));
		for (const FBPVariableDoc& Var : Doc.Variables)
		{
			FString Line = FString::Printf(TEXT("%s: %s"), *Var.Name, *Var.Type);
			if (!Var.DefaultValue.IsEmpty())
			{
				Line += FString::Printf(TEXT(" = %s"), *Var.DefaultValue);
			}
			Lines.Add(Line);
		}
		Lines.Add(TEXT(""));
	}
	
	// Components
	if (Doc.Components.Num() > 0)
	{
		Lines.Add(TEXT("## Components"));
		Lines.Add(TEXT(""));
		for (const FBPComponentDoc& Comp : Doc.Components)
		{
			Lines.Add(FString::Printf(TEXT("%s: %s"), *Comp.Name, *Comp.Type));
		}
		Lines.Add(TEXT(""));
	}
	
	// Dependencies
	if (Doc.Dependencies.Num() > 0)
	{
		Lines.Add(TEXT("## Dependencies"));
		Lines.Add(TEXT(""));
		for (const FString& Dep : Doc.Dependencies)
		{
			Lines.Add(Dep);
		}
		Lines.Add(TEXT(""));
	}
	
	// Events
	for (const FBPFunctionDoc& Event : Doc.Events)
	{
		Lines.Add(FString::Printf(TEXT("## Event: %s"), *Event.Name));
		Lines.Add(TEXT(""));
		GenerateStatements(Event.Statements, 0, Lines);
		Lines.Add(TEXT(""));
	}
	
	// Functions
	for (const FBPFunctionDoc& Func : Doc.Functions)
	{
		Lines.Add(FString::Printf(TEXT("## Function: %s"), *Func.Name));
		Lines.Add(TEXT(""));
		GenerateStatements(Func.Statements, 0, Lines);
		Lines.Add(TEXT(""));
	}
	
	return FString::Join(Lines, TEXT("\n"));
}

void FBlueprintDocGenerator::GenerateStatements(const TArray<TSharedPtr<FBPStatement>>& Statements, int32 IndentLevel, TArray<FString>& OutLines)
{
	for (const TSharedPtr<FBPStatement>& Stmt : Statements)
	{
		if (!Stmt.IsValid())
		{
			continue;
		}
		
		const FString IndentStr = Indent(IndentLevel);
		
		switch (Stmt->Type)
		{
		case EBPStmtType::Assignment:
			OutLines.Add(FString::Printf(TEXT("%s%s%s = %s"), *IndentStr, *Stmt->Prefix, *Stmt->Target, *RenderExpression(Stmt->Expr)));
			break;
		
		case EBPStmtType::FuncCall:
			OutLines.Add(FString::Printf(TEXT("%s%s%s"), *IndentStr, *Stmt->Prefix, *RenderExpression(Stmt->Expr)));
			break;
		
		case EBPStmtType::IfStatement:
			OutLines.Add(FString::Printf(TEXT("%sIF %s:"), *IndentStr, *RenderExpression(Stmt->Expr)));
			
			if (Stmt->Body.Num() > 0)
			{
				GenerateStatements(Stmt->Body, IndentLevel + 1, OutLines);
			}
			else
			{
				OutLines.Add(Indent(IndentLevel + 1) + TEXT("..."));
			}
			
			if (Stmt->ElseBody.Num() > 0)
			{
				OutLines.Add(FString::Printf(TEXT("%sELSE:"), *IndentStr));
				GenerateStatements(Stmt->ElseBody, IndentLevel + 1, OutLines);
			}
			break;
		
		case EBPStmtType::Sequence:
			OutLines.Add(FString::Printf(TEXT("%sSEQUENCE:"), *IndentStr));
			for (const TArray<TSharedPtr<FBPStatement>>& Branch : Stmt->Branches)
			{
				GenerateStatements(Branch, IndentLevel + 1, OutLines);
			}
			break;
		
		case EBPStmtType::Raw:
			OutLines.Add(FString::Printf(TEXT("%s%s"), *IndentStr, *Stmt->RawText));
			break;
		}
	}
}

FString FBlueprintDocGenerator::RenderExpression(const TSharedPtr<FBPExpression>& Expr)
{
	if (!Expr.IsValid())
	{
		return TEXT("?");
	}
	
	switch (Expr->Type)
	{
	case EBPExprType::Literal:
		return Expr->bQuoted ? FString::Printf(TEXT("\"%s\""), *Expr->Text) : Expr->Text;
	
	case EBPExprType::VariableRef:
		return Expr->Owner.IsEmpty() ? Expr->Text : FString::Printf(TEXT("%s.%s"), *Expr->Owner, *Expr->Text);
	
	case EBPExprType::FuncCall:
	{
		TArray<FString> ArgStrings;
		for (const TSharedPtr<FBPExpression>& Arg : Expr->Args)
		{
			ArgStrings.Add(RenderExpression(Arg));
		}
		const FString CallText = FString::Printf(TEXT("%s(%s)"), *Expr->Text, *FString::Join(ArgStrings, TEXT(", ")));
		return Expr->Owner.IsEmpty() ? CallText : FString::Printf(TEXT("%s.%s"), *Expr->Owner, *CallText);
	}
	
	case EBPExprType::BinaryOp:
		// 一元运算（如 NOT）：Left 为空
		if (!Expr->Left.IsValid())
		{
			return FString::Printf(TEXT("%s %s"), *Expr->Text, *RenderExpression(Expr->Right));
		}
		return FString::Printf(TEXT("%s %s %s"), *RenderExpression(Expr->Left), *Expr->Text, *RenderExpression(Expr->Right));
	
	case EBPExprType::CastExpr:
		return FString::Printf(TEXT("Cast(%s, %s)"), *RenderExpression(Expr->Left), *Expr->SubType);
	
	case EBPExprType::PropertyAccess:
		return FString::Printf(TEXT("%s.%s"), *Expr->Owner, *Expr->Text);
	
	case EBPExprType::ArrayAccess:
		return FString::Printf(TEXT("%s[%s]"), *RenderExpression(Expr->Left), *RenderExpression(Expr->Right));
	
	case EBPExprType::SelfRef:
		return TEXT("Self");
	
	case EBPExprType::NoneRef:
		return TEXT("None");
	}
	
	return TEXT("?");
}

FString FBlueprintDocGenerator::Indent(int32 Level)
{
	FString Result;
	for (int32 i = 0; i < Level; ++i)
	{
		Result += TEXT("    ");
	}
	return Result;
}
