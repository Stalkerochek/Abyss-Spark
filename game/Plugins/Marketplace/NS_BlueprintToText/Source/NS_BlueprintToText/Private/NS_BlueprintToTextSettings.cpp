// Copyright NodeSmith 2026

#include "NS_BlueprintToTextSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "NS_BlueprintToTextSettings"

UNS_BlueprintToTextSettings::UNS_BlueprintToTextSettings()
{
	HttpApiURL = TEXT("http://localhost:11434/api/generate");
	ModelName = TEXT("gemma4:e4b");
	SourceLanguage = TEXT("English");
	TargetLanguage = TEXT("Chinese");
	RetryCount = 2;
	RequestTimeout = 10;
}

FName UNS_BlueprintToTextSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FText UNS_BlueprintToTextSettings::GetSectionText() const
{
	return LOCTEXT("SettingsSection", "NS Blueprint To Text");
}

void UNS_BlueprintToTextSettings::EnsureFolderAliasesLoaded() const
{
	if (bFolderAliasesLoaded)
	{
		return;
	}
	
	// const_cast：LoadFolderAliasConfig 会写入 mutable 成员
	const_cast<UNS_BlueprintToTextSettings*>(this)->LoadFolderAliasConfig();
	bFolderAliasesLoaded = true;
}

void UNS_BlueprintToTextSettings::SetFolderAlias(const FString& FolderPath, const FString& Alias)
{
	EnsureFolderAliasesLoaded();
	FolderAliases.Add(FolderPath, Alias);
	SaveFolderAliasConfig();
}

FString UNS_BlueprintToTextSettings::GetFolderAlias(const FString& FolderPath) const
{
	EnsureFolderAliasesLoaded();
	const FString* Found = FolderAliases.Find(FolderPath);
	return Found ? *Found : FString();
}

bool UNS_BlueprintToTextSettings::HasFolderAlias(const FString& FolderPath) const
{
	EnsureFolderAliasesLoaded();
	return FolderAliases.Contains(FolderPath);
}

void UNS_BlueprintToTextSettings::RemoveFolderAlias(const FString& FolderPath)
{
	EnsureFolderAliasesLoaded();
	FolderAliases.Remove(FolderPath);
	SaveFolderAliasConfig();
}

void UNS_BlueprintToTextSettings::RemoveFolderAliases(const TArray<FString>& FolderPaths)
{
	EnsureFolderAliasesLoaded();
	bool bChanged = false;
	for (const FString& FolderPath : FolderPaths)
	{
		if (FolderAliases.Remove(FolderPath) > 0)
		{
			bChanged = true;
		}
	}
	
	if (bChanged)
	{
		SaveFolderAliasConfig();
	}
}

void UNS_BlueprintToTextSettings::SaveFolderAliasConfig()
{
	// 获取FolderAlias.ini文件路径
	FString ConfigPath = FPaths::ProjectConfigDir() / TEXT("FolderAlias.ini");
	
	// 使用 FConfigFile 直接写入（GConfig 对非标准 ini 文件标记 NoSave，Flush 不会落盘）
	FConfigFile ConfigFile;
	
	for (const TPair<FString, FString>& Pair : FolderAliases)
	{
		FString Key = FString::Printf(TEXT("FolderAliases_%s"), *Pair.Key);
		ConfigFile.SetString(
			TEXT("/Script/NS_BlueprintToText.NS_BlueprintToTextSettings"),
			*Key,
			*Pair.Value
		);
	}
	
	if (ConfigFile.Write(ConfigPath))
	{
		UE_LOG(LogTemp, Log, TEXT("已保存 %d 个文件夹备注到: %s"), FolderAliases.Num(), *ConfigPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("保存文件夹备注失败: %s"), *ConfigPath);
	}
}

void UNS_BlueprintToTextSettings::LoadFolderAliasConfig()
{
	// 获取FolderAlias.ini文件路径
	FString ConfigPath = FPaths::ProjectConfigDir() / TEXT("FolderAlias.ini");
	
	if (!FPaths::FileExists(ConfigPath))
	{
		UE_LOG(LogTemp, Log, TEXT("FolderAlias.ini 不存在，将在第一次设置备注时创建"));
		return;
	}
	
	// 清空现有数据
	FolderAliases.Empty();
	
	// 使用 FConfigFile 直接读取
	FConfigFile ConfigFile;
	ConfigFile.Read(ConfigPath);
	
	const FConfigSection* Section = ConfigFile.FindOrAddConfigSection(TEXT("/Script/NS_BlueprintToText.NS_BlueprintToTextSettings"));
	if (!Section)
	{
		return;
	}
	
	for (const TPair<FName, FConfigValue>& Pair : *Section)
	{
		const FString Key = Pair.Key.ToString();
		// 移除 FolderAliases_ 前缀获取实际路径
		if (Key.StartsWith(TEXT("FolderAliases_")))
		{
			FString FolderPath = Key.RightChop(14); // 14 = "FolderAliases_".Len()
			FolderAliases.Add(FolderPath, Pair.Value.GetValue());
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("已从 %s 加载 %d 个文件夹备注"), *ConfigPath, FolderAliases.Num());
}

#undef LOCTEXT_NAMESPACE
