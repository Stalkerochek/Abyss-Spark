// Copyright NodeSmith 2026

#include "NS_BlueprintToText.h"
#include "BlueprintDocTypes.h"
#include "BlueprintAnalyzer.h"
#include "BlueprintDocGenerator.h"
#include "BlueprintCommentTranslator.h"
#include "FolderAliasManager.h"
#include "FolderAliasCustomization.h"
#include "NS_BlueprintToTextSettings.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ToolMenus.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "ContentBrowserDataMenuContexts.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Composite.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "UObject/UnrealType.h"
#include "UObject/FieldIterator.h"
#include "Components/SceneComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_AddComponent.h"
#include "K2Node_DynamicCast.h"

#define LOCTEXT_NAMESPACE "FNS_BlueprintToTextModule"

void FNS_BlueprintToTextModule::StartupModule()
{
	// 注册文件夹别名定制
	FFolderAliasCustomization::Register();
	
	// 等待ToolMenus模块加载
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FNS_BlueprintToTextModule::RegisterMenuExtensions));
}

void FNS_BlueprintToTextModule::ShutdownModule()
{
	// 注销文件夹别名定制
	FFolderAliasCustomization::Unregister();
	
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FNS_BlueprintToTextModule::RegisterMenuExtensions()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// 注册内容浏览器文件夹右键菜单
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.FolderContextMenu");
	if (Menu)
	{
		FToolMenuSection& Section = Menu->FindOrAddSection("PathContextBulkOperations");
		Section.AddDynamicEntry("BlueprintToText", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
		{
			// 从当前菜单上下文获取选中的路径
			UContentBrowserDataMenuContext_FolderMenu* Context = InSection.FindContext<UContentBrowserDataMenuContext_FolderMenu>();
			
			if (Context && Context->SelectedItems.Num() > 0)
			{
				FString FolderPath = Context->SelectedItems[0].GetVirtualPath().ToString();
				
				InSection.AddMenuEntry(
					"ExportBlueprintToText",
					LOCTEXT("ExportBlueprintToText", "Blueprint To Text"),
					LOCTEXT("ExportBlueprintToTextTooltip", "导出此文件夹中的蓝图为文本格式"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, FolderPath]()
					{
						OnBlueprintToTextClicked(FolderPath);
					}))
				);
				
				InSection.AddMenuEntry(
					"TranslateFolderBlueprints",
					LOCTEXT("TranslateFolderBlueprints", "Translate Blueprint Comments"),
					LOCTEXT("TranslateFolderBlueprintsTooltip", "翻译此文件夹中所有蓝图的注释"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, FolderPath]()
					{
						OnTranslateFolderBlueprintsClicked(FolderPath);
					}))
				);
				
				InSection.AddMenuEntry(
					"SetFolderAlias",
					LOCTEXT("SetFolderAlias", "Set Folder Alias"),
					LOCTEXT("SetFolderAliasTooltip", "设置文件夹备注"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([FolderPath]()
					{
						FFolderAliasManager::ShowSetAliasDialog(FolderPath);
					}))
				);
				
				InSection.AddMenuEntry(
					"BatchTranslateFolderNames",
					LOCTEXT("BatchTranslateFolderNames", "Batch Translate Folder Names"),
					LOCTEXT("BatchTranslateFolderNamesTooltip", "批量翻译此文件夹及其子文件夹的名称并设置为备注"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([FolderPath]()
					{
						FFolderAliasManager::BatchTranslateFolderNames(FolderPath);
					}))
				);
				
				InSection.AddMenuEntry(
					"CleanInvalidFolderAliases",
					LOCTEXT("CleanInvalidFolderAliases", "Clean Invalid Folder Notes"),
					LOCTEXT("CleanInvalidFolderAliasesTooltip", "清理已被删除文件夹的失效备注"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						const int32 CleanedCount = FFolderAliasManager::CleanInvalidFolderAliases();
						FNotificationInfo Info(FText::FromString(FString::Printf(
							TEXT("已清理 %d 条失效备注"), CleanedCount)));
						Info.ExpireDuration = 3.0f;
						FSlateNotificationManager::Get().AddNotification(Info);
					}))
				);
			}
		}));
	}
	
	// 注册内容浏览器资源右键菜单
	UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
	if (AssetMenu)
	{
		FToolMenuSection& AssetSection = AssetMenu->FindOrAddSection("GetAssetActions");
		AssetSection.AddDynamicEntry("BlueprintAssetActions", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
		{
			// 添加"Blueprint To Text"菜单项
			InSection.AddMenuEntry(
				"ExportSelectedAssetsToText",
				LOCTEXT("ExportSelectedAssetsToText", "Blueprint To Text"),
				LOCTEXT("ExportSelectedAssetsToTextTooltip", "导出选中的蓝图/数据表为文本格式"),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this]()
				{
					OnExportSelectedAssetsClicked();
				}))
			);
			
			// 添加"Translate Blueprint Comments"菜单项
			InSection.AddMenuEntry(
				"TranslateSelectedBlueprints",
				LOCTEXT("TranslateSelectedBlueprints", "Translate Blueprint Comments"),
				LOCTEXT("TranslateSelectedBlueprintsTooltip", "翻译选中蓝图的注释"),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this]()
				{
					OnTranslateSelectedBlueprintsClicked();
				}))
			);
		}));
	}
}

void FNS_BlueprintToTextModule::OnBlueprintToTextClicked(const FString& FolderPath)
{
	if (FolderPath.IsEmpty())
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("开始处理文件夹: %s"), *FolderPath);
	ProcessSelectedFolder(FolderPath);
}

void FNS_BlueprintToTextModule::ProcessSelectedFolder(const FString& SelectedPath)
{
	// 处理路径：移除 /All 前缀（如果存在）
	FString AssetPath = SelectedPath;
	if (AssetPath.StartsWith(TEXT("/All/")))
	{
		AssetPath = AssetPath.RightChop(4); // 移除 "/All"
	}
	
	UE_LOG(LogTemp, Log, TEXT("处理后的资源路径: %s"), *AssetPath);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// 确保AssetRegistry已扫描完成
	AssetRegistry.SearchAllAssets(true);

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssetsByPath(FName(*AssetPath), AllAssets, true);

	UE_LOG(LogTemp, Log, TEXT("找到 %d 个资源"), AllAssets.Num());

	if (AllAssets.Num() == 0)
	{
		FNotificationInfo Info(LOCTEXT("NoAssets", "选择的文件夹中没有找到资产"));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	FScopedSlowTask SlowTask(AllAssets.Num(), LOCTEXT("ExportingAssets", "导出资产为文本..."));
	SlowTask.MakeDialog();

	FString OutputDir = FPaths::ProjectContentDir() / TEXT("BlueprintToText");
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	int32 SuccessCount = 0;

	for (const FAssetData& Asset : AllAssets)
	{
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("处理: %s"), *Asset.AssetName.ToString())));

		if (ExportAssetToJson(Asset, OutputDir))
		{
			SuccessCount++;
		}
	}

	// 显示完成通知
	FText ResultText = FText::FromString(FString::Printf(TEXT("成功导出 %d/%d 个蓝图资产到 Content/BlueprintToText/"), SuccessCount, AllAssets.Num()));
	FNotificationInfo Info(ResultText);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("BlueprintToText: 成功导出 %d/%d 个资产到 %s"), SuccessCount, AllAssets.Num(), *OutputDir);
}

bool FNS_BlueprintToTextModule::ExportAssetToJson(const FAssetData& AssetData, const FString& OutputDir)
{
	UObject* Asset = AssetData.GetAsset();
	if (!Asset)
	{
		return false;
	}

	FString ClassName = AssetData.AssetClassPath.ToString();
	FString OutputPath = OutputDir / AssetData.AssetName.ToString() + TEXT(".txt");

	// 优先处理数据表（避免被蓝图类型捕获）
	if (UDataTable* DataTable = Cast<UDataTable>(Asset))
	{
		UE_LOG(LogTemp, Log, TEXT("处理数据表: %s"), *AssetData.AssetName.ToString());
		return ProcessDataTable(DataTable, OutputPath);
	}
	
	// 处理动画蓝图（AnimBlueprint 继承自 Blueprint，需要先检查）
	if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Asset))
	{
		UE_LOG(LogTemp, Log, TEXT("处理动画蓝图: %s"), *AssetData.AssetName.ToString());
		return ProcessBlueprint(AnimBP, OutputPath);
	}
	
	// 处理普通蓝图类型
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (BP)
	{
		UE_LOG(LogTemp, Log, TEXT("处理蓝图: %s (类型: %s)"), *AssetData.AssetName.ToString(), *ClassName);
		return ProcessBlueprint(BP, OutputPath);
	}
	
	// 处理其他可能的蓝图类型（通过类名判断）
	if (ClassName.Contains(TEXT("Blueprint")))
	{
		BP = Cast<UBlueprint>(Asset);
		if (BP)
		{
			UE_LOG(LogTemp, Log, TEXT("处理蓝图（通过类名）: %s (类型: %s)"), *AssetData.AssetName.ToString(), *ClassName);
			return ProcessBlueprint(BP, OutputPath);
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("不支持的资源类型: %s (类型: %s)"), *AssetData.AssetName.ToString(), *ClassName);
	return false;
}

bool FNS_BlueprintToTextModule::ProcessBlueprint(UBlueprint* Blueprint, const FString& OutputPath)
{
	if (!Blueprint)
	{
		return false;
	}

	// 结构化伪代码转换管线：
	// Blueprint → 分析器(Exec Flow + Data Flow) → IR → 伪代码生成器 → 文本
	FBPDocument Document = FBlueprintAnalyzer::Analyze(Blueprint);
	const FString TextContent = FBlueprintDocGenerator::Generate(Document);

	// 保存为纯文本文件
	return FFileHelper::SaveStringToFile(TextContent, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FNS_BlueprintToTextModule::ProcessDataTable(UDataTable* DataTable, const FString& OutputPath)
{
	if (!DataTable)
	{
		UE_LOG(LogTemp, Error, TEXT("数据表指针为空"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("开始处理数据表: %s"), *DataTable->GetName());

	TArray<FString> ContentItems;
	
	// 提取所有行名称
	TArray<FName> RowNames = DataTable->GetRowNames();
	
	UE_LOG(LogTemp, Log, TEXT("数据表 %s 包含 %d 行"), *DataTable->GetName(), RowNames.Num());
	
	if (RowNames.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("数据表 %s 没有行数据"), *DataTable->GetName());
		// 即使没有数据也创建空文件
		return FFileHelper::SaveStringToFile(TEXT(""), *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	
	for (const FName& RowName : RowNames)
	{
		FString RowNameStr = RowName.ToString();
		ContentItems.Add(RowNameStr);
		UE_LOG(LogTemp, Log, TEXT("  行: %s"), *RowNameStr);
	}
	
	// 合并所有内容为逗号分隔的文本
	FString TextContent = FString::Join(ContentItems, TEXT("，"));
	
	UE_LOG(LogTemp, Log, TEXT("数据表内容长度: %d 字符"), TextContent.Len());
	
	// 保存为纯文本文件
	bool bSuccess = FFileHelper::SaveStringToFile(TextContent, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	
	if (bSuccess)
	{
		UE_LOG(LogTemp, Log, TEXT("✓ 数据表 %s 导出成功: %s"), *DataTable->GetName(), *OutputPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("✗ 数据表 %s 导出失败: %s"), *DataTable->GetName(), *OutputPath);
	}
	
	return bSuccess;
}

void FNS_BlueprintToTextModule::OnExportSelectedAssetsClicked()
{
	UE_LOG(LogTemp, Log, TEXT("=== 开始导出选中的资源 ==="));
	ProcessSelectedAssets();
}

void FNS_BlueprintToTextModule::ProcessSelectedAssets()
{
	// 获取选中的资源
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	UE_LOG(LogTemp, Log, TEXT("选中的资源数量: %d"), SelectedAssets.Num());

	if (SelectedAssets.Num() == 0)
	{
		FNotificationInfo Info(LOCTEXT("NoAssetsSelected", "请先选择要导出的资源"));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		UE_LOG(LogTemp, Warning, TEXT("没有选中任何资源"));
		return;
	}

	// 创建输出目录
	FString OutputDir = FPaths::ProjectContentDir() / TEXT("BlueprintToText");
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	// 显示进度条
	FScopedSlowTask SlowTask(SelectedAssets.Num(), LOCTEXT("ExportingSelectedAssets", "导出选中的资源为文本..."));
	SlowTask.MakeDialog();

	int32 SuccessCount = 0;
	int32 SkippedCount = 0;

	for (const FAssetData& AssetData : SelectedAssets)
	{
		SlowTask.EnterProgressFrame(1.0f, FText::FromString(FString::Printf(TEXT("处理: %s"), *AssetData.AssetName.ToString())));

		UE_LOG(LogTemp, Log, TEXT("检查资源: %s, 类型: %s"), *AssetData.AssetName.ToString(), *AssetData.AssetClassPath.ToString());

		// 检查是否是支持的类型
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(LogTemp, Warning, TEXT("无法加载资源: %s"), *AssetData.AssetName.ToString());
			SkippedCount++;
			continue;
		}

		bool bIsSupported = false;
		
		// 检查是否是动画蓝图
		if (Cast<UAnimBlueprint>(Asset))
		{
			bIsSupported = true;
			UE_LOG(LogTemp, Log, TEXT("  → 动画蓝图"));
		}
		// 检查是否是蓝图
		else if (Cast<UBlueprint>(Asset))
		{
			bIsSupported = true;
			UE_LOG(LogTemp, Log, TEXT("  → 蓝图"));
		}
		// 检查是否是数据表
		else if (Cast<UDataTable>(Asset))
		{
			bIsSupported = true;
			UE_LOG(LogTemp, Log, TEXT("  → 数据表"));
		}
		// 检查类名是否包含Blueprint
		else if (AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint")))
		{
			bIsSupported = true;
			UE_LOG(LogTemp, Log, TEXT("  → 蓝图类型（通过类名）"));
		}

		if (bIsSupported)
		{
			if (ExportAssetToJson(AssetData, OutputDir))
			{
				SuccessCount++;
				UE_LOG(LogTemp, Log, TEXT("✓ 成功导出: %s"), *AssetData.AssetName.ToString());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("✗ 导出失败: %s"), *AssetData.AssetName.ToString());
				SkippedCount++;
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("跳过不支持的资源类型: %s (类型: %s)"), 
				*AssetData.AssetName.ToString(), *AssetData.AssetClassPath.ToString());
			SkippedCount++;
		}
	}

	// 显示完成通知
	FText ResultText;
	if (SkippedCount > 0)
	{
		ResultText = FText::FromString(FString::Printf(
			TEXT("成功导出 %d/%d 个资源到 Content/BlueprintToText/\n跳过 %d 个不支持的资源"), 
			SuccessCount, SelectedAssets.Num(), SkippedCount));
	}
	else
	{
		ResultText = FText::FromString(FString::Printf(
			TEXT("成功导出 %d/%d 个资源到 Content/BlueprintToText/"), 
			SuccessCount, SelectedAssets.Num()));
	}
	
	FNotificationInfo Info(ResultText);
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogTemp, Log, TEXT("=== 导出完成: 成功 %d, 跳过 %d, 总计 %d ==="), 
		SuccessCount, SkippedCount, SelectedAssets.Num());
}

void FNS_BlueprintToTextModule::OnTranslateSelectedBlueprintsClicked()
{
	FBlueprintCommentTranslator::TranslateSelectedBlueprints();
}

void FNS_BlueprintToTextModule::OnTranslateFolderBlueprintsClicked(const FString& FolderPath)
{
	// 处理路径：移除 /All 前缀（如果存在）
	FString AssetPath = FolderPath;
	if (AssetPath.StartsWith(TEXT("/All/")))
	{
		AssetPath = AssetPath.RightChop(4); // 移除 "/All"
	}
	
	FBlueprintCommentTranslator::TranslateBlueprintsInFolder(AssetPath);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FNS_BlueprintToTextModule, NS_BlueprintToText)