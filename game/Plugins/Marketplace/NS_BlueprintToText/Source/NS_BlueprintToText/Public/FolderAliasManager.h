// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"

/**
 * 文件夹备注管理器
 * 负责处理文件夹备注的设置、显示和批量翻译
 */
class FFolderAliasManager
{
public:
	/** 显示设置备注的对话框 */
	static void ShowSetAliasDialog(const FString& FolderPath);
	
	/** 批量翻译文件夹名并设置为备注 */
	static void BatchTranslateFolderNames(const FString& RootFolderPath);
	
	/** 获取文件夹的显示名称（如果有备注则返回备注，否则返回原名） */
	static FString GetFolderDisplayName(const FString& FolderPath);
	
	/** 移除文件夹备注 */
	static void RemoveFolderAlias(const FString& FolderPath);
	
	/** 清理失效备注（对应文件夹已被删除），返回清理的数量 */
	static int32 CleanInvalidFolderAliases();
	
	/** 截断文本（添加省略号） */
	static FString TruncateText(const FString& Text, int32 MaxLength = 20);
	
	/** 截断文本到最多2行（超过则添加省略号） */
	static FString TruncateToTwoLines(const FString& Text);

private:
	/** 翻译单个文件夹名 */
	static void TranslateSingleFolderName(const FString& FolderPath, const FString& FolderName);
	
	/** 获取文件夹下的所有子文件夹 */
	static void GetAllSubFolders(const FString& RootPath, TArray<FString>& OutFolders);
};
