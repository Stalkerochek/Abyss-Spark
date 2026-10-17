// Copyright NodeSmith 2026

#include "BlueprintCommentTranslator.h"
#include "NS_BlueprintToTextSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BlueprintCommentTranslator"

// 初始化静态变量
bool FBlueprintCommentTranslator::bIsTranslating = false;
int32 FBlueprintCommentTranslator::TotalCommentsToTranslate = 0;
int32 FBlueprintCommentTranslator::TotalTranslatedComments = 0;

void FBlueprintCommentTranslator::TranslateSelectedBlueprints()
{
	// 检查是否正在翻译
	if (bIsTranslating)
	{
		UE_LOG(LogTemp, Warning, TEXT("翻译任务正在进行中，请等待当前任务完成"));
		
		// 使用通知替代对话框
		FNotificationInfo Info(LOCTEXT("TranslationInProgress", "翻译任务正在进行中，请等待当前任务完成"));
		Info.ExpireDuration = 3.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("============ 开始翻译选中的蓝图 ============"));
	
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	UE_LOG(LogTemp, Log, TEXT("选中的资源数量: %d"), SelectedAssets.Num());

	if (SelectedAssets.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("没有选中任何资源"));
		
		// 使用通知替代对话框
		FNotificationInfo Info(LOCTEXT("NoAssetsSelected", "请先选择要翻译的蓝图资源"));
		Info.ExpireDuration = 3.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	// 第一步：统计所有需要翻译的注释数量
	TArray<UBlueprint*> BlueprintsToTranslate;
	int32 TotalComments = 0;
	
	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (AssetData.GetClass()->IsChildOf(UBlueprint::StaticClass()))
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (Blueprint)
			{
				// 统计此蓝图中需要翻译的注释数量
				TArray<UEdGraph*> AllGraphs;
				Blueprint->GetAllGraphs(AllGraphs);
				
				TSet<FString> UniqueComments;
				for (UEdGraph* Graph : AllGraphs)
				{
					if (Graph)
					{
						for (UEdGraphNode* Node : Graph->Nodes)
						{
							if (Node && !Node->NodeComment.IsEmpty() && !Node->NodeComment.Contains(TEXT("\n")))
							{
								FString Comment = Node->NodeComment;
								// 检查是否需要翻译
								if (ShouldTranslateComment(Comment))
								{
									UniqueComments.Add(Comment);
								}
							}
						}
					}
				}
				
				if (UniqueComments.Num() > 0)
				{
					BlueprintsToTranslate.Add(Blueprint);
					TotalComments += UniqueComments.Num();
				}
			}
		}
	}

	if (BlueprintsToTranslate.Num() == 0 || TotalComments == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("未找到需要翻译的注释"));
		
		FNotificationInfo Info(LOCTEXT("NoCommentsToTranslate", "未找到需要翻译的注释"));
		Info.ExpireDuration = 3.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	// 设置翻译标志和计数器
	bIsTranslating = true;
	TotalCommentsToTranslate = TotalComments;
	TotalTranslatedComments = 0;

	// 显示第一次通知：开始翻译
	FText StartMessage = FText::Format(
		LOCTEXT("TranslationStart", "开始翻译，共 {0} 条需要翻译"),
		FText::AsNumber(TotalComments)
	);
	
	FNotificationInfo Info(StartMessage);
	Info.ExpireDuration = 3.0f;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("开始翻译 %d 个蓝图，共 %d 条注释"), BlueprintsToTranslate.Num(), TotalComments);

	// 第二步：开始顺序翻译所有蓝图（一个接一个）
	TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr = MakeShared<TArray<UBlueprint*>>(BlueprintsToTranslate);
	TSharedPtr<int32> BlueprintIndexPtr = MakeShared<int32>(0);
	TranslateNextBlueprint(BlueprintsPtr, BlueprintIndexPtr);
}

void FBlueprintCommentTranslator::TranslateBlueprintsInFolder(const FString& FolderPath)
{
	// 检查是否正在翻译
	if (bIsTranslating)
	{
		UE_LOG(LogTemp, Warning, TEXT("翻译任务正在进行中，请等待当前任务完成"));
		
		// 使用通知替代对话框
		FNotificationInfo Info(LOCTEXT("TranslationInProgress", "翻译任务正在进行中，请等待当前任务完成"));
		Info.ExpireDuration = 3.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("=== 开始翻译文件夹中的蓝图 ==="));
	UE_LOG(LogTemp, Log, TEXT("文件夹路径: %s"), *FolderPath);
	
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssetsByPath(FName(*FolderPath), AssetDataList, true);

	UE_LOG(LogTemp, Log, TEXT("文件夹中找到 %d 个资源"), AssetDataList.Num());

	// 第一步：统计所有需要翻译的注释数量
	TArray<UBlueprint*> BlueprintsToTranslate;
	int32 TotalComments = 0;
	
	for (const FAssetData& AssetData : AssetDataList)
	{
		UClass* AssetClass = AssetData.GetClass();
		if (AssetClass && AssetClass->IsChildOf(UBlueprint::StaticClass()))
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (Blueprint)
			{
				// 统计此蓝图中需要翻译的注释数量
				TArray<UEdGraph*> AllGraphs;
				Blueprint->GetAllGraphs(AllGraphs);
				
				TSet<FString> UniqueComments;
				for (UEdGraph* Graph : AllGraphs)
				{
					if (Graph)
					{
						for (UEdGraphNode* Node : Graph->Nodes)
						{
							if (Node && !Node->NodeComment.IsEmpty() && !Node->NodeComment.Contains(TEXT("\n")))
							{
								FString Comment = Node->NodeComment;
								// 检查是否需要翻译
								if (ShouldTranslateComment(Comment))
								{
									UniqueComments.Add(Comment);
								}
							}
						}
					}
				}
				
				if (UniqueComments.Num() > 0)
				{
					BlueprintsToTranslate.Add(Blueprint);
					TotalComments += UniqueComments.Num();
				}
			}
		}
	}

	if (BlueprintsToTranslate.Num() == 0 || TotalComments == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("文件夹中未找到需要翻译的注释"));
		
		FNotificationInfo Info(LOCTEXT("NoCommentsInFolder", "文件夹中未找到需要翻译的注释"));
		Info.ExpireDuration = 3.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	// 设置翻译标志和计数器
	bIsTranslating = true;
	TotalCommentsToTranslate = TotalComments;
	TotalTranslatedComments = 0;

	// 显示第一次通知：开始翻译
	FText StartMessage = FText::Format(
		LOCTEXT("TranslationStart", "开始翻译，共 {0} 条需要翻译"),
		FText::AsNumber(TotalComments)
	);
	
	FNotificationInfo Info(StartMessage);
	Info.ExpireDuration = 3.0f;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("开始翻译文件夹中 %d 个蓝图，共 %d 条注释"), BlueprintsToTranslate.Num(), TotalComments);

	// 第二步：开始顺序翻译所有蓝图（一个接一个）
	TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr = MakeShared<TArray<UBlueprint*>>(BlueprintsToTranslate);
	TSharedPtr<int32> BlueprintIndexPtr = MakeShared<int32>(0);
	TranslateNextBlueprint(BlueprintsPtr, BlueprintIndexPtr);
}

void FBlueprintCommentTranslator::TranslateNextBlueprint(
	TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr,
	TSharedPtr<int32> BlueprintIndexPtr)
{
	// 检查是否所有蓝图都已处理完成
	if (*BlueprintIndexPtr >= BlueprintsPtr->Num())
	{
		UE_LOG(LogTemp, Log, TEXT("所有蓝图处理完成"));
		return;
	}

	// 获取当前要翻译的蓝图
	UBlueprint* Blueprint = (*BlueprintsPtr)[*BlueprintIndexPtr];
	
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Error, TEXT("蓝图指针为空，跳过"));
		(*BlueprintIndexPtr)++;
		TranslateNextBlueprint(BlueprintsPtr, BlueprintIndexPtr);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("正在处理蓝图 [%d/%d]: %s"), *BlueprintIndexPtr + 1, BlueprintsPtr->Num(), *Blueprint->GetName());

	// 收集所有需要翻译的注释
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	TArray<FString> CommentArray;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !Node->NodeComment.IsEmpty() && !Node->NodeComment.Contains(TEXT("\n")))
			{
				FString Comment = Node->NodeComment;
				// 检查是否需要翻译
				if (ShouldTranslateComment(Comment) && !CommentArray.Contains(Comment))
				{
					CommentArray.Add(Comment);
				}
			}
		}
	}

	if (CommentArray.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("蓝图 %s 中没有找到需要翻译的注释，跳过"), *Blueprint->GetName());
		(*BlueprintIndexPtr)++;
		TranslateNextBlueprint(BlueprintsPtr, BlueprintIndexPtr);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("蓝图 %s 中找到 %d 个唯一注释，开始顺序翻译..."), *Blueprint->GetName(), CommentArray.Num());

	// 创建共享数据
	TSharedPtr<TArray<FString>> CommentArrayPtr = MakeShared<TArray<FString>>(CommentArray);
	TSharedPtr<int32> CurrentIndexPtr = MakeShared<int32>(0);
	TSharedPtr<int32> SuccessCountPtr = MakeShared<int32>(0);
	int32 TotalCount = CommentArray.Num();
	FString BlueprintName = Blueprint->GetName();

	// 开始翻译当前蓝图的第一个注释
	TranslateNextCommentInSequence(Blueprint, CommentArrayPtr, CurrentIndexPtr, SuccessCountPtr, 
		TotalCount, BlueprintName, BlueprintsPtr, BlueprintIndexPtr);
}

void FBlueprintCommentTranslator::TranslateNextCommentInSequence(
	UBlueprint* Blueprint,
	TSharedPtr<TArray<FString>> CommentArrayPtr,
	TSharedPtr<int32> CurrentIndexPtr,
	TSharedPtr<int32> SuccessCountPtr,
	int32 TotalCount,
	FString BlueprintName,
	TSharedPtr<TArray<UBlueprint*>> BlueprintsPtr,
	TSharedPtr<int32> BlueprintIndexPtr)
{
	// 检查当前蓝图的所有注释是否都已翻译完成
	if (*CurrentIndexPtr >= TotalCount)
	{
		UE_LOG(LogTemp, Log, TEXT("蓝图 %s 所有注释翻译完成！"), *BlueprintName);
		
		// 标记蓝图已修改并保存
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->PostEditChange();
		
		// 移动到下一个蓝图
		(*BlueprintIndexPtr)++;
		
		// 检查是否所有蓝图都已处理完成
		if (*BlueprintIndexPtr >= BlueprintsPtr->Num())
		{
			// 重置翻译标志
			bIsTranslating = false;
			UE_LOG(LogTemp, Log, TEXT("所有翻译任务完成！"));
			
			// 显示第二次通知：翻译完成
			FText CompletionMessage = FText::Format(
				LOCTEXT("TranslationCompleted", "翻译完成：{0}/{1}"),
				FText::AsNumber(TotalTranslatedComments),
				FText::AsNumber(TotalCommentsToTranslate)
			);
			
			FNotificationInfo Info(CompletionMessage);
			Info.ExpireDuration = 5.0f;
			Info.bFireAndForget = true;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
		else
		{
			// 继续翻译下一个蓝图
			TranslateNextBlueprint(BlueprintsPtr, BlueprintIndexPtr);
		}
		
		return;
	}

	// 获取当前要翻译的注释
	const FString& CurrentComment = (*CommentArrayPtr)[*CurrentIndexPtr];
	int32 CurrentIndex = *CurrentIndexPtr;

	UE_LOG(LogTemp, Log, TEXT("========================================="));
	UE_LOG(LogTemp, Log, TEXT("[蓝图 %s] [注释 %d/%d]  准备翻译文本: %s"), 
		*BlueprintName,
		CurrentIndex + 1, TotalCount, *CurrentComment.Left(50));

	// 翻译当前注释
	TranslateText(CurrentComment, [Blueprint, CommentArrayPtr, CurrentIndexPtr, SuccessCountPtr, 
		TotalCount, BlueprintName, BlueprintsPtr, BlueprintIndexPtr, CurrentComment](const FString& TranslatedText)
	{
		if (TranslatedText.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("翻译失败或返回空结果，跳过此注释"));
		}
		else
		{
			// 立即更新蓝图中所有匹配的注释
			UpdateSingleComment(Blueprint, CurrentComment, TranslatedText);
			(*SuccessCountPtr)++;
			TotalTranslatedComments++;
		}

		// 移动到下一个注释
		(*CurrentIndexPtr)++;

		// 继续翻译当前蓝图的下一个注释
		TranslateNextCommentInSequence(Blueprint, CommentArrayPtr, CurrentIndexPtr, SuccessCountPtr, 
			TotalCount, BlueprintName, BlueprintsPtr, BlueprintIndexPtr);
	});
}

void FBlueprintCommentTranslator::TranslateText(const FString& OriginalText, TFunction<void(const FString&)> OnComplete, int32 RetryAttempt)
{
	const UNS_BlueprintToTextSettings* Settings = GetDefault<UNS_BlueprintToTextSettings>();
	if (!Settings)
	{
		UE_LOG(LogTemp, Error, TEXT("无法获取插件设置"));
		OnComplete(TEXT(""));
		return;
	}

	if (RetryAttempt > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("重试翻译 (第 %d 次重试): %s"), RetryAttempt, *OriginalText.Left(50));
	}
	
	UE_LOG(LogTemp, Log, TEXT("使用配置 - URL: %s, 模型: %s, 源语言: %s, 目标语言: %s, 超时: %d秒"), 
		*Settings->HttpApiURL, *Settings->ModelName, *Settings->SourceLanguage, *Settings->TargetLanguage, Settings->RequestTimeout);

	// 创建HTTP请求
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Settings->HttpApiURL);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetTimeout(Settings->RequestTimeout);

	// 构建请求体
	TSharedPtr<FJsonObject> RequestObj = MakeShared<FJsonObject>();
	RequestObj->SetStringField(TEXT("model"), Settings->ModelName);
	
	FString Prompt = FString::Printf(TEXT("Translate the following text from %s to %s . Provide only the best translation without explanations or reasoning:\n\n%s"),
		*Settings->SourceLanguage, *Settings->TargetLanguage, *OriginalText);
	RequestObj->SetStringField(TEXT("prompt"), Prompt);
	RequestObj->SetBoolField(TEXT("stream"), false);

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);



	HttpRequest->SetContentAsString(RequestBody);

	// 绑定回调
	HttpRequest->OnProcessRequestComplete().BindStatic(&FBlueprintCommentTranslator::OnTranslationRequestComplete, OnComplete, OriginalText, RetryAttempt);

	// 发送请求
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("HTTP请求发送失败"));
		
		// 检查是否需要重试
		if (RetryAttempt < Settings->RetryCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("准备重试..."));
			TranslateText(OriginalText, OnComplete, RetryAttempt + 1);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("已达到最大重试次数 (%d)，放弃翻译"), Settings->RetryCount);
			OnComplete(TEXT(""));
		}
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("HTTP请求已发送，等待响应..."));
	}
}

void FBlueprintCommentTranslator::OnTranslationRequestComplete(
	FHttpRequestPtr Request, 
	FHttpResponsePtr Response, 
	bool bWasSuccessful, 
	TFunction<void(const FString&)> OnComplete, 
	FString OriginalText,
	int32 RetryAttempt)
{
	const UNS_BlueprintToTextSettings* Settings = GetDefault<UNS_BlueprintToTextSettings>();
	if (!Settings)
	{
		UE_LOG(LogTemp, Error, TEXT("无法获取插件设置"));
		OnComplete(TEXT(""));
		return;
	}

	// 检查请求是否成功
	if (!bWasSuccessful)
	{
		UE_LOG(LogTemp, Error, TEXT("HTTP请求失败 - 网络错误或超时"));
		
		// 检查是否需要重试
		if (RetryAttempt < Settings->RetryCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("准备重试... (第 %d/%d 次)"), RetryAttempt + 1, Settings->RetryCount);
			TranslateText(OriginalText, OnComplete, RetryAttempt + 1);
			return;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("已达到最大重试次数 (%d)，放弃翻译"), Settings->RetryCount);
			OnComplete(TEXT(""));
			return;
		}
	}

	if (!Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("HTTP响应无效"));
		
		// 检查是否需要重试
		if (RetryAttempt < Settings->RetryCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("准备重试... (第 %d/%d 次)"), RetryAttempt + 1, Settings->RetryCount);
			TranslateText(OriginalText, OnComplete, RetryAttempt + 1);
			return;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("已达到最大重试次数 (%d)，放弃翻译"), Settings->RetryCount);
			OnComplete(TEXT(""));
			return;
		}
	}

	int32 ResponseCode = Response->GetResponseCode();
	UE_LOG(LogTemp, Log, TEXT("收到HTTP响应，状态码: %d"), ResponseCode);

	if (ResponseCode != 200)
	{
		UE_LOG(LogTemp, Error, TEXT("HTTP请求返回错误状态码: %d"), ResponseCode);
		UE_LOG(LogTemp, Error, TEXT("响应内容: %s"), *Response->GetContentAsString());
		
		// 检查是否需要重试（服务器错误5xx或请求超时408）
		bool bShouldRetry = (ResponseCode >= 500 && ResponseCode < 600) || ResponseCode == 408 || ResponseCode == 429;
		
		if (bShouldRetry && RetryAttempt < Settings->RetryCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("服务器错误，准备重试... (第 %d/%d 次)"), RetryAttempt + 1, Settings->RetryCount);
			TranslateText(OriginalText, OnComplete, RetryAttempt + 1);
			return;
		}
		else
		{
			if (bShouldRetry)
			{
				UE_LOG(LogTemp, Error, TEXT("已达到最大重试次数 (%d)，放弃翻译"), Settings->RetryCount);
			}
			OnComplete(TEXT(""));
			return;
		}
	}

	FString ResponseBody = Response->GetContentAsString();
	UE_LOG(LogTemp, Log, TEXT("响应内容长度: %d"), ResponseBody.Len());
	UE_LOG(LogTemp, Log, TEXT("响应内容预览: %s"), *ResponseBody.Left(200));

	FString TranslatedText = ParseOllamaResponse(ResponseBody);

	if (TranslatedText.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("解析翻译结果失败，原文: %s"), *OriginalText.Left(50));
		
		// 解析失败也尝试重试
		if (RetryAttempt < Settings->RetryCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("解析失败，准备重试... (第 %d/%d 次)"), RetryAttempt + 1, Settings->RetryCount);
			TranslateText(OriginalText, OnComplete, RetryAttempt + 1);
			return;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("已达到最大重试次数 (%d)，放弃翻译"), Settings->RetryCount);
			OnComplete(TEXT(""));
			return;
		}
	}
	else
	{
		if (RetryAttempt > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("重试成功！(第 %d 次重试)"), RetryAttempt);
		}
		UE_LOG(LogTemp, Log, TEXT("翻译成功！"));
		UE_LOG(LogTemp, Log, TEXT("  原文: %s"), *OriginalText.Left(50));
		UE_LOG(LogTemp, Log, TEXT("  译文: %s"), *TranslatedText.Left(50));
	}

	OnComplete(TranslatedText);
}

FString FBlueprintCommentTranslator::ParseOllamaResponse(const FString& ResponseBody)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		FString Response;
		if (JsonObject->TryGetStringField(TEXT("response"), Response))
		{
			UE_LOG(LogTemp, Log, TEXT("成功解析Ollama响应"));
			return Response.TrimStartAndEnd();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("JSON中未找到'response'字段"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("JSON解析失败"));
	}

	return TEXT("");
}

bool FBlueprintCommentTranslator::ShouldTranslateComment(const FString& Comment)
{
	// 去除首尾空白
	FString TrimmedComment = Comment.TrimStartAndEnd();
	
	// 空字符串不翻译
	if (TrimmedComment.IsEmpty())
	{
		return false;
	}
	
	// 检查是否只包含数字、符号和空白
	bool bHasLetter = false;
	for (TCHAR Char : TrimmedComment)
	{
		// 检查是否是字母（包括中文、日文、韩文等Unicode字符）
		if (FChar::IsAlpha(Char) || Char > 127)  // ASCII > 127 通常是非英文字符
		{
			bHasLetter = true;
			break;
		}
	}
	
	// 如果没有字母，说明只有数字、符号和空白，不需要翻译
	if (!bHasLetter)
	{
		UE_LOG(LogTemp, Log, TEXT("跳过纯数字/符号注释: %s"), *TrimmedComment);
		return false;
	}
	
	return true;
}

void FBlueprintCommentTranslator::UpdateSingleComment(UBlueprint* Blueprint, const FString& OriginalComment, const FString& TranslatedText)
{
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Error, TEXT("更新注释时，蓝图指针为空"));
		return;
	}

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	int32 UpdatedCount = 0;
	
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeComment == OriginalComment)
			{
				// 组合原文和译文：原文 + 换行 + 译文
				FString NewComment = FString::Printf(TEXT("%s\n%s"), *OriginalComment, *TranslatedText);
				Node->NodeComment = NewComment;
				Node->Modify();
				UpdatedCount++;
			}
		}
	}

	if (UpdatedCount > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("  更新了 %d 个节点的注释"), UpdatedCount);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("  未找到匹配的节点注释"));
	}
}

#undef LOCTEXT_NAMESPACE
