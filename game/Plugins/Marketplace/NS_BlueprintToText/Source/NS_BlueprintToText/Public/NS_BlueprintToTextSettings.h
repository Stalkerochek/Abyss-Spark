// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "NS_BlueprintToTextSettings.generated.h"

/**
 * 插件翻译设置
 */
UCLASS(config=EditorPerProjectUserSettings, meta=(DisplayName="NS Blueprint To Text"))
class NS_BLUEPRINTTOTEXT_API UNS_BlueprintToTextSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UNS_BlueprintToTextSettings();

	/** HTTP API URL (支持 Ollama、llama.cpp 等兼容接口) */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "HTTP API URL", ToolTip = "翻译服务的HTTP API地址，支持Ollama、llama.cpp等兼容接口"))
	FString HttpApiURL;

	/** 模型名称 */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "Model Name"))
	FString ModelName;

	/** 源语言 */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "Source Language"))
	FString SourceLanguage;

	/** 目标语言 */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "Target Language"))
	FString TargetLanguage;

	/** 请求超时重试次数 */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "Retry Count", ClampMin = "0", ClampMax = "10", ToolTip = "HTTP请求失败时的重试次数"))
	int32 RetryCount;

	/** 请求超时时间（秒） */
	UPROPERTY(Config, EditAnywhere, Category = "Translation", meta = (DisplayName = "Request Timeout (seconds)", ClampMin = "5", ClampMax = "300", ToolTip = "HTTP请求的超时时间"))
	int32 RequestTimeout;

	/** 文件夹备注映射 (路径 -> 备注) - 存储在单独的FolderAlias.ini文件中，由代码手动读写（懒加载），不作为UPROPERTY以免被引擎Config系统干扰 */
	mutable TMap<FString, FString> FolderAliases;

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
	virtual FText GetSectionText() const override;
	
	// 备注管理函数
	void SetFolderAlias(const FString& FolderPath, const FString& Alias);
	FString GetFolderAlias(const FString& FolderPath) const;
	bool HasFolderAlias(const FString& FolderPath) const;
	void RemoveFolderAlias(const FString& FolderPath);
	/** 批量移除多个文件夹备注（只保存一次配置） */
	void RemoveFolderAliases(const TArray<FString>& FolderPaths);
	/** 确保备注已从文件加载（懒加载，首次访问时执行） */
	void EnsureFolderAliasesLoaded() const;
	
private:
	/** 保存别名配置到FolderAlias.ini */
	void SaveFolderAliasConfig();
	
	/** 加载别名配置从FolderAlias.ini */
	void LoadFolderAliasConfig();
	
	/** 备注是否已从文件加载 */
	mutable bool bFolderAliasesLoaded = false;
};
