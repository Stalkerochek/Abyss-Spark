// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

/**
 * 蓝图注释翻译工具
 */
class FBlueprintCommentTranslator
{
public:
	/** 翻译选中的蓝图资源 */
	static void TranslateSelectedBlueprints();

	/** 翻译指定文件夹中的所有蓝图 */
	static void TranslateBlueprintsInFolder(const FString& FolderPath);

private:
	/** 是否正在翻译 */
	static bool bIsTranslating;

	/** 总共需要翻译的注释数量 */
	static int32 TotalCommentsToTranslate;

	/** 已成功翻译的注释数量 */
	static int32 TotalTranslatedComments;

	/** 顺序翻译下一个蓝图 */
	static void TranslateNextBlueprint(
		TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr,
		TSharedPtr<int32> BlueprintIndexPtr);

	/** 顺序翻译下一个注释（支持多蓝图顺序） */
	static void TranslateNextCommentInSequence(
		class UBlueprint* Blueprint,
		TSharedPtr<TArray<FString>> CommentArrayPtr,
		TSharedPtr<int32> CurrentIndexPtr,
		TSharedPtr<int32> SuccessCountPtr,
		int32 TotalCount,
		FString BlueprintName,
		TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr,
		TSharedPtr<int32> BlueprintIndexPtr);

	/** 调用大模型API进行翻译 */
	static void TranslateText(const FString& OriginalText, TFunction<void(const FString&)> OnComplete, int32 RetryAttempt = 0);

	/** HTTP请求完成回调 */
	static void OnTranslationRequestComplete(
		FHttpRequestPtr Request, 
		FHttpResponsePtr Response, 
		bool bWasSuccessful, 
		TFunction<void(const FString&)> OnComplete, 
		FString OriginalText,
		int32 RetryAttempt);

	/** 解析Ollama响应 */
	static FString ParseOllamaResponse(const FString& ResponseBody);

	/** 判断注释是否需要翻译 */
	static bool ShouldTranslateComment(const FString& Comment);

	/** 更新单个注释 */
	static void UpdateSingleComment(class UBlueprint* Blueprint, const FString& OriginalComment, const FString& TranslatedText);
};
