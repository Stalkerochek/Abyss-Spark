// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "BlueprintDocTypes.h"

/**
 * 伪代码生成器：将 FBPDocument IR 转换为结构化伪代码文本
 */
class FBlueprintDocGenerator
{
public:
	/** 生成完整文档文本 */
	static FString Generate(const FBPDocument& Doc);

private:
	static void GenerateStatements(const TArray<TSharedPtr<FBPStatement>>& Statements, int32 IndentLevel, TArray<FString>& OutLines);
	static FString RenderExpression(const TSharedPtr<FBPExpression>& Expr);
	static FString Indent(int32 Level);
};
