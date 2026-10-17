// Copyright NodeSmith 2026

#include "FolderAliasManager.h"
#include "NS_BlueprintToTextSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

#define LOCTEXT_NAMESPACE "FolderAliasManager"

void FFolderAliasManager::ShowSetAliasDialog(const FString& FolderPath)
{
	// 处理路径
	FString CleanPath = FolderPath;
	if (CleanPath.StartsWith(TEXT("/All/")))
	{
		CleanPath = CleanPath.RightChop(4);
	}
	
	// 获取文件夹名称
	FString FolderName = FPaths::GetCleanFilename(CleanPath);
	
	// 获取当前别名
	UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
	FString CurrentAlias = Settings->GetFolderAlias(CleanPath);
	
	TSharedPtr<SEditableTextBox> AliasTextBox;
	TSharedPtr<STextBlock> InfoTextBlock;
	TSharedPtr<SWindow> Window;
	
	// 先创建窗口（不带内容）。
	// 注意：不能把内容直接写在 SAssignNew/SNew 的 [] 槽里——C++17 下 <<= 右侧（内容）
	// 先于左侧 Expose 求值，按钮回调按值捕获到的 Window 仍是空指针，导致窗口无法关闭。
	// 因此先拿到 Window，再通过 SetContent 设置内容，保证回调捕获到有效指针。
	SAssignNew(Window, SWindow)
		.Title(LOCTEXT("SetFolderAlias", "设置文件夹备注"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	
	// 窗口已创建，此时 Window 有效，回调可正常关闭窗口
	Window->SetContent(
		SNew(SBox)
		.Padding(FMargin(10))
		.MinDesiredWidth(400)
		[
				SNew(SVerticalBox)
				
				// 文件夹路径显示
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 10)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("文件夹: %s"), *FolderName)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				
				// 提示信息
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 5)
				[
					SAssignNew(InfoTextBlock, STextBlock)
					.Text(LOCTEXT("AliasInfo", "备注将保存到项目配置，可通过右键菜单或设置面板查看"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
					.AutoWrapText(true)
				]
				
				// 当前备注显示
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 5)
				[
					SNew(STextBlock)
					.Text(CurrentAlias.IsEmpty() ? 
						LOCTEXT("NoAlias", "当前无备注") : 
						FText::FromString(FString::Printf(TEXT("当前备注: %s"), *CurrentAlias)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(CurrentAlias.IsEmpty() ? 
						FLinearColor(0.5f, 0.5f, 0.5f, 1.0f) : 
						FLinearColor(1.0f, 0.9f, 0.3f, 1.0f))
				]
				
				// 备注输入框
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 5, 0, 10)
				[
					SAssignNew(AliasTextBox, SEditableTextBox)
					.Text(FText::FromString(CurrentAlias))
					.HintText(LOCTEXT("AliasHint", "输入备注（例如：角色、武器、环境等）"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
				
				// 按钮
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					
					// 确定按钮
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(0, 0, 5, 0)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("OK", "确定"))
						.OnClicked_Lambda([AliasTextBox, CleanPath, Window]() -> FReply
						{
							FString NewAlias = AliasTextBox->GetText().ToString().TrimStartAndEnd();
							
							UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
							if (NewAlias.IsEmpty())
							{
								Settings->RemoveFolderAlias(CleanPath);
								
								FNotificationInfo Info(LOCTEXT("AliasRemoved", "已移除文件夹备注"));
								Info.ExpireDuration = 2.0f;
								FSlateNotificationManager::Get().AddNotification(Info);
							}
							else
							{
								Settings->SetFolderAlias(CleanPath, NewAlias);
								
								FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("已设置备注: %s"), *NewAlias)));
								Info.ExpireDuration = 2.0f;
								FSlateNotificationManager::Get().AddNotification(Info);
							}
							
							// 关闭窗口
							if (Window.IsValid())
							{
								Window->RequestDestroyWindow();
							}
							
							return FReply::Handled();
						})
					]
					
					// 移除按钮
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(5, 0, 5, 0)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("Remove", "移除备注"))
						.IsEnabled(!CurrentAlias.IsEmpty())
						.OnClicked_Lambda([CleanPath, Window]() -> FReply
						{
							UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
							Settings->RemoveFolderAlias(CleanPath);
							
							FNotificationInfo Info(LOCTEXT("AliasRemoved", "已移除文件夹备注"));
							Info.ExpireDuration = 2.0f;
							FSlateNotificationManager::Get().AddNotification(Info);
							
							// 关闭窗口
							if (Window.IsValid())
							{
								Window->RequestDestroyWindow();
							}
							
							return FReply::Handled();
						})
					]
					
					// 取消按钮
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(5, 0, 0, 0)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("Cancel", "取消"))
						.OnClicked_Lambda([Window]() -> FReply
						{
							// 关闭窗口
							if (Window.IsValid())
							{
								Window->RequestDestroyWindow();
							}
							
							return FReply::Handled();
						})
					]
				]
			]
		);
	
	FSlateApplication::Get().AddWindow(Window.ToSharedRef());
}

void FFolderAliasManager::BatchTranslateFolderNames(const FString& RootFolderPath)
{
	// 处理路径
	FString CleanPath = RootFolderPath;
	if (CleanPath.StartsWith(TEXT("/All/")))
	{
		CleanPath = CleanPath.RightChop(4);
	}
	
	// 获取所有子文件夹
	TArray<FString> AllFolders;
	GetAllSubFolders(CleanPath, AllFolders);
	
	// 包含当前文件夹
	AllFolders.Insert(CleanPath, 0);
	
	if (AllFolders.Num() == 0)
	{
		FNotificationInfo Info(LOCTEXT("NoFolders", "没有找到文件夹"));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}
	
	// 显示开始通知
	FNotificationInfo StartInfo(FText::FromString(FString::Printf(TEXT("开始批量翻译文件夹名，共 %d 个文件夹"), AllFolders.Num())));
	StartInfo.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(StartInfo);
	
	UE_LOG(LogTemp, Log, TEXT("开始批量翻译 %d 个文件夹"), AllFolders.Num());
	
	// 创建进度条
	FScopedSlowTask SlowTask(AllFolders.Num(), LOCTEXT("TranslatingFolders", "正在翻译文件夹名..."));
	SlowTask.MakeDialog();
	
	int32 SuccessCount = 0;
	int32 SkippedCount = 0;
	
	// 逐个翻译
	for (const FString& FolderPath : AllFolders)
	{
		FString FolderName = FPaths::GetCleanFilename(FolderPath);
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("翻译: %s"), *FolderName)));
		
		// 检查是否已有别名
		UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
		if (Settings->HasFolderAlias(FolderPath))
		{
			UE_LOG(LogTemp, Log, TEXT("跳过已有备注的文件夹: %s"), *FolderName);
			SkippedCount++;
			continue;
		}
		
		// 翻译文件夹名
		TranslateSingleFolderName(FolderPath, FolderName);
		SuccessCount++;
		
		// 避免请求过快
		FPlatformProcess::Sleep(0.1f);
	}
	
	// 显示完成通知
	FText ResultText = FText::FromString(FString::Printf(
		TEXT("批量翻译完成\n成功: %d, 跳过: %d, 总计: %d"),
		SuccessCount, SkippedCount, AllFolders.Num()));
	
	FNotificationInfo Info(ResultText);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	
	UE_LOG(LogTemp, Log, TEXT("批量翻译完成 - 成功: %d, 跳过: %d"), SuccessCount, SkippedCount);
}

FString FFolderAliasManager::GetFolderDisplayName(const FString& FolderPath)
{
	UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
	FString Alias = Settings->GetFolderAlias(FolderPath);
	
	if (!Alias.IsEmpty())
	{
		return TruncateText(Alias);
	}
	
	return FPaths::GetCleanFilename(FolderPath);
}

void FFolderAliasManager::RemoveFolderAlias(const FString& FolderPath)
{
	UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
	Settings->RemoveFolderAlias(FolderPath);
}

int32 FFolderAliasManager::CleanInvalidFolderAliases()
{
	UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
	Settings->EnsureFolderAliasesLoaded();
	
	// 收集已失效（文件夹已被删除）的备注路径
	TArray<FString> InvalidPaths;
	for (const TPair<FString, FString>& Pair : Settings->FolderAliases)
	{
		const FString& FolderPath = Pair.Key;
		
		// 将 /Game/xxx 形式的包路径转换为磁盘路径，检查目录是否仍然存在
		FString DiskPath;
		bool bExists = false;
		if (FPackageName::TryConvertLongPackageNameToFilename(FolderPath, DiskPath))
		{
			bExists = IFileManager::Get().DirectoryExists(*DiskPath);
		}
		
		if (!bExists)
		{
			InvalidPaths.Add(FolderPath);
		}
	}
	
	if (InvalidPaths.Num() > 0)
	{
		Settings->RemoveFolderAliases(InvalidPaths);
	}
	
	return InvalidPaths.Num();
}

void FFolderAliasManager::TranslateSingleFolderName(const FString& FolderPath, const FString& FolderName)
{
	UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
	
	// 构建翻译提示词
	FString Prompt = FString::Printf(
		TEXT("Translate the following text from %s to %s. Provide only the best translation without explanations or reasoning:\n\n%s"),
		*Settings->SourceLanguage,
		*Settings->TargetLanguage,
		*FolderName
	);
	
	// 构建JSON请求体
	TSharedPtr<FJsonObject> RequestObj = MakeShareable(new FJsonObject());
	RequestObj->SetStringField(TEXT("model"), Settings->ModelName);
	RequestObj->SetStringField(TEXT("prompt"), Prompt);
	RequestObj->SetBoolField(TEXT("stream"), false);
	
	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);
	
	// 创建HTTP请求
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Settings->HttpApiURL);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetContentAsString(RequestBody);
	HttpRequest->SetTimeout(Settings->RequestTimeout);
	
	// 使用同步方式处理（在批量翻译的循环中）
	bool bRequestSent = false;
	FString TranslatedText;
	
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[FolderPath, FolderName, &bRequestSent, &TranslatedText](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
		{
			bRequestSent = true;
			
			if (bSuccess && Response.IsValid())
			{
				FString ResponseStr = Response->GetContentAsString();
				
				TSharedPtr<FJsonObject> JsonObject;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
				
				if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
				{
					if (JsonObject->HasField(TEXT("response")))
					{
						TranslatedText = JsonObject->GetStringField(TEXT("response")).TrimStartAndEnd();
						
						if (!TranslatedText.IsEmpty())
						{
							// 保存别名
							UNS_BlueprintToTextSettings* Settings = GetMutableDefault<UNS_BlueprintToTextSettings>();
							Settings->SetFolderAlias(FolderPath, TranslatedText);
							
							UE_LOG(LogTemp, Log, TEXT("✓ 翻译成功: %s -> %s"), *FolderName, *TranslatedText);
						}
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("✗ 翻译失败: %s"), *FolderName);
			}
		}
	);
	
	HttpRequest->ProcessRequest();
	
	// 等待请求完成（最多等待超时时间）
	float WaitTime = 0.0f;
	float MaxWaitTime = Settings->RequestTimeout + 1.0f;
	while (!bRequestSent && WaitTime < MaxWaitTime)
	{
		FPlatformProcess::Sleep(0.1f);
		WaitTime += 0.1f;
	}
}

void FFolderAliasManager::GetAllSubFolders(const FString& RootPath, TArray<FString>& OutFolders)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 确保AssetRegistry已扫描完成
	AssetRegistry.SearchAllAssets(true);
	
	TArray<FString> SubPaths;
	AssetRegistry.GetSubPaths(RootPath, SubPaths, true);
	
	OutFolders.Append(SubPaths);
}

FString FFolderAliasManager::TruncateText(const FString& Text, int32 MaxLength)
{
	if (Text.Len() <= MaxLength)
	{
		return Text;
	}
	
	return Text.Left(MaxLength - 3) + TEXT("...");
}

FString FFolderAliasManager::TruncateToTwoLines(const FString& Text)
{
	// 按字符数估算行数（假设每行约20个字符）
	const int32 MaxCharsPerLine = 20;
	const int32 MaxTotalChars = MaxCharsPerLine * 2; // 2行
	
	if (Text.Len() <= MaxTotalChars)
	{
		return Text;
	}
	
	// 截断到最大字符数并添加省略号
	return Text.Left(MaxTotalChars - 3) + TEXT("...");
}

#undef LOCTEXT_NAMESPACE
