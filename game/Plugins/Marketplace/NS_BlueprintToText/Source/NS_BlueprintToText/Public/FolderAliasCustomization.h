// Copyright NodeSmith 2026

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class SWidget;

/**
 * Content Browser 文件夹别名显示定制
 *
 * UE5.6 的 Content Browser 没有为文件夹提供官方的悬停提示扩展点
 * （FAssetViewExtraStateGenerator / OnGetCustomAssetToolTip 仅对资产文件生效）。
 * 本类通过定期扫描 Slate widget 树，找到资产视图中的文件夹项控件
 * （SAssetViewItem 及其派生类，引擎公开头文件 AssetViewWidgets.h），
 * 将其悬停提示替换为 "引擎默认信息 + 文件夹别名" 的增强版本。
 *
 * 提示内容在每次打开时动态生成：
 * - 别名修改后无需刷新，下次悬停即生效
 * - widget 被列表复用时也不会显示过期别名
 */
class FFolderAliasCustomization
{
public:
	/** 注册Content Browser扩展 */
	static void Register();

	/** 注销Content Browser扩展 */
	static void Unregister();

private:
	/** ticker 回调：扫描窗口 widget 树并替换文件夹悬停提示 */
	static void InstallFolderAliasToolTips();

	/** 递归遍历 widget 树 */
	static void WalkWidgetsInstallToolTips(const TSharedRef<SWidget>& InWidget);

	/** 尝试为资产视图项控件替换悬停提示（仅处理文件夹项） */
	static void TryInstallFolderAliasToolTip(const TSharedRef<SWidget>& InWidget);

	/** 安装器的 ticker 句柄 */
	static FTSTicker::FDelegateHandle TooltipInstallerTickerHandle;

	/** 已替换悬停提示的文件夹项控件（弱引用，用于去重与注销时还原） */
	static TArray<TWeakPtr<SWidget>> HandledFolderItemWidgets;
};
