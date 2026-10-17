// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FNS_BlueprintToTextModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenuExtensions();
	void OnBlueprintToTextClicked(const FString& FolderPath);
	void OnExportSelectedAssetsClicked();
	void ProcessSelectedFolder(const FString& SelectedPath);
	void ProcessSelectedAssets();
	bool ExportAssetToJson(const struct FAssetData& AssetData, const FString& OutputDir);
	
	// 资产类型的处理函数
	bool ProcessBlueprint(class UBlueprint* Blueprint, const FString& OutputPath);
	bool ProcessDataTable(class UDataTable* DataTable, const FString& OutputPath);
	
	// 翻译功能
	void OnTranslateSelectedBlueprintsClicked();
	void OnTranslateFolderBlueprintsClicked(const FString& FolderPath);
};
