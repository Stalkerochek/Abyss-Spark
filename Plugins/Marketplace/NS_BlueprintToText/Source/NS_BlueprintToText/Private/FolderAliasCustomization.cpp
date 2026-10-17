// Copyright NodeSmith 2026

#include "FolderAliasCustomization.h"
#include "NS_BlueprintToTextSettings.h"
// 引擎公开头文件：SAssetViewItem 及其派生类（SAssetListItem/SAssetTileItem/SAssetColumnItem）
#include "AssetViewWidgets.h"
// FAssetViewItem（GetItem 为 CONTENTBROWSER_API 导出）
#include "AssetViewTypes.h"
// FContentBrowserItem（IsFolder/GetDisplayName/GetVirtualPath/GetInternalPath 均已导出）
#include "ContentBrowserItem.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/ChildrenBase.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "HAL/IConsoleManager.h"
#include "UObject/NameTypes.h"

#define LOCTEXT_NAMESPACE "FolderAliasCustomization"

namespace
{
	/**
	 * 通过派生类的成员指针访问 SAssetViewItem 的受保护成员 AssetItem
	 * （成员指针一旦获得即不再受访问级别限制，且 AssetViewWidgets.h 为引擎公开头文件）
	 */
	class FAssetViewItemAccess : public SAssetViewItem
	{
	public:
		using FAssetItemPtr = TSharedPtr<FAssetViewItem> SAssetViewItem::*;

		static FAssetItemPtr AssetItemPtr()
		{
			return &FAssetViewItemAccess::AssetItem;
		}
	};

	/** 资产视图项控件的类型名（均派生自 SAssetViewItem） */
	bool IsAssetViewItemTypeName(const FName InType)
	{
		return InType == TEXT("SAssetViewItem")
			|| InType == TEXT("SAssetListItem")
			|| InType == TEXT("SAssetTileItem")
			|| InType == TEXT("SAssetColumnItem");
	}

	/** 与引擎内部 UE::Editor::ContentBrowser::IsNewStyleEnabled() 等价的判断 */
	bool IsNewStyleEnabledCompat()
	{
		if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ContentBrowser.EnableNewStyle")))
		{
			return CVar->GetBool();
		}
		return false;
	}

	/** 按内部路径（如 /Game/Characters）查询文件夹别名 */
	FString GetFolderAliasForItem(const FContentBrowserItem& InItem)
	{
		const FName InternalPath = InItem.GetInternalPath();
		if (InternalPath.IsNone())
		{
			return FString();
		}

		const FString FolderPath = FNameBuilder(InternalPath).ToString();
		const UNS_BlueprintToTextSettings* Settings = GetDefault<UNS_BlueprintToTextSettings>();
		return Settings ? Settings->GetFolderAlias(FolderPath) : FString();
	}

	/**
	 * 文件夹悬停提示：自行构建等价于引擎默认的文件夹 tooltip 内容，
	 * 并在底部追加别名信息。内容在每次打开时动态生成。
	 */
	class FFolderAliasItemToolTip : public SToolTip
	{
	public:
		SLATE_BEGIN_ARGS(FFolderAliasItemToolTip) {}
			/** 对应的资产视图项控件（SAssetViewItem） */
			SLATE_ARGUMENT(TSharedPtr<SWidget>, TargetItemWidget)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			TargetItemWidget = InArgs._TargetItemWidget;

			if (IsNewStyleEnabledCompat())
			{
				SToolTip::Construct(
					SToolTip::FArguments()
					.TextMargin(FMargin(12.f, 8.f, 12.f, 2.f))
					.BorderImage(FAppStyle::GetBrush(TEXT("AssetThumbnail.Tooltip.Border"))));
			}
			else
			{
				SToolTip::Construct(SToolTip::FArguments().TextMargin(FMargin(1.0f)));
			}
		}

		//~ IToolTip interface
		virtual bool IsEmpty() const override
		{
			return !TargetItemWidget.IsValid();
		}

		virtual void OnOpening() override
		{
			const TSharedPtr<SWidget> PinnedWidget = TargetItemWidget.Pin();
			if (!PinnedWidget.IsValid())
			{
				return;
			}

			SAssetViewItem* ItemWidget = static_cast<SAssetViewItem*>(PinnedWidget.Get());
			const TSharedPtr<FAssetViewItem> AssetItem = ItemWidget->*FAssetViewItemAccess::AssetItemPtr();
			if (!AssetItem.IsValid())
			{
				return;
			}

			// FAssetViewItem::GetItem() 已由 CONTENTBROWSER_API 导出
			const FContentBrowserItem& ContentItem = AssetItem->GetItem();

			// FContentBrowserItem::IsFolder() 已由 CONTENTBROWSERDATA_API 导出
			if (!ContentItem.IsFolder())
			{
				return;
			}

			const FText FolderName = ContentItem.GetDisplayName();
			const FName VirtualPath = ContentItem.GetVirtualPath();
			const FName InternalPath = ContentItem.GetInternalPath();

			const FString Alias = GetFolderAliasForItem(ContentItem);

			// 构建路径信息行
			TSharedRef<SVerticalBox> InfoBox = SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("PathLabel", "Path:"))
						.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.0f))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromName(VirtualPath))
						.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
					]
				];

			TSharedRef<SVerticalBox> ContentWidget = [&]() -> TSharedRef<SVerticalBox>
			{
				if (IsNewStyleEnabledCompat())
				{
					return SNew(SVerticalBox)
						// 文件夹名称
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 0.f, 0.f, 6.f)
						[
							SNew(STextBlock)
							.Text(FolderName)
							.ColorAndOpacity(FStyleColors::White)
							.Font(FAppStyle::GetFontStyle(TEXT("ContentBrowser.Tooltip.EntryFont")))
						]
						// 文件夹图标 + Folder 标签
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.f, 0.f, 4.f, 0.f)
							[
								SNew(SBox)
								.WidthOverride(16.f)
								.HeightOverride(16.f)
								[
									SNew(SImage)
									.Image(FAppStyle::GetBrush(TEXT("ContentBrowser.ListViewFolderIcon")))
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Font(FAppStyle::GetFontStyle(TEXT("ContentBrowser.Tooltip.EntryFont")))
								.Text(LOCTEXT("FolderLabel", "Folder"))
							]
						]
						// 分隔线
						+ SVerticalBox::Slot()
						.Padding(FMargin(0.f, 6.f))
						.AutoHeight()
						[
							SNew(SSeparator)
							.Orientation(Orient_Horizontal)
							.Thickness(1.f)
							.ColorAndOpacity(COLOR("#484848FF"))
							.SeparatorImage(FAppStyle::Get().GetBrush(TEXT("WhiteBrush")))
						]
						// 路径信息
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							InfoBox
						];
				}
				else
				{
					return SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.f, 0.f, 0.f, 4.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.f, 0.f, 4.f, 0.f)
							[
								SNew(STextBlock)
								.Text(FolderName)
								.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("FolderBracketed", "(Folder)"))
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							InfoBox
						];
				}
			}();

			// 有别名时追加别名信息
			if (!Alias.IsEmpty())
			{
				ContentWidget->AddSlot()
					.AutoHeight()
					.Padding(IsNewStyleEnabledCompat() ? FMargin(0.f, 6.f, 0.f, 0.f) : FMargin(0.f, 4.f, 0.f, 0.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.f, 0.f, 4.f, 0.f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AliasLabel", "备注:"))
							.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(Alias))
							.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10))
							.ColorAndOpacity(FLinearColor(1.0f, 0.9f, 0.3f, 1.0f))
							.AutoWrapText(true)
							.WrapTextAt(400.0f)
						]
					];
			}

			SetContentWidget(ContentWidget);
		}

		virtual void OnClosed() override
		{
			ResetContentWidget();
		}

	private:
		/** 对应的 SAssetViewItem 控件（弱引用） */
		TWeakPtr<SWidget> TargetItemWidget;
	};
}

FTSTicker::FDelegateHandle FFolderAliasCustomization::TooltipInstallerTickerHandle;
TArray<TWeakPtr<SWidget>> FFolderAliasCustomization::HandledFolderItemWidgets;

void FFolderAliasCustomization::Register()
{
	TooltipInstallerTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("NS_FolderAliasTooltipInstaller"), 0.25f,
		[](float)
		{
			InstallFolderAliasToolTips();
			return true;
		});

	UE_LOG(LogTemp, Log, TEXT("文件夹备注悬停提示已启用：悬停资产视图中的文件夹即可查看备注"));
}

void FFolderAliasCustomization::Unregister()
{
	if (TooltipInstallerTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TooltipInstallerTickerHandle);
		TooltipInstallerTickerHandle.Reset();
	}

	for (const TWeakPtr<SWidget>& WeakWidget : HandledFolderItemWidgets)
	{
		if (const TSharedPtr<SWidget> Widget = WeakWidget.Pin())
		{
			Widget->SetToolTip(nullptr);
		}
	}
	HandledFolderItemWidgets.Empty();

	UE_LOG(LogTemp, Log, TEXT("FFolderAliasCustomization: 已注销"));
}

void FFolderAliasCustomization::InstallFolderAliasToolTips()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	HandledFolderItemWidgets.RemoveAll([](const TWeakPtr<SWidget>& InWeak)
	{
		return !InWeak.IsValid();
	});

	for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetTopLevelWindows())
	{
		WalkWidgetsInstallToolTips(Window);
	}
}

void FFolderAliasCustomization::WalkWidgetsInstallToolTips(const TSharedRef<SWidget>& InWidget)
{
	TryInstallFolderAliasToolTip(InWidget);

	if (FChildren* Children = InWidget->GetChildren())
	{
		const int32 NumChildren = Children->Num();
		for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
		{
			WalkWidgetsInstallToolTips(Children->GetChildAt(ChildIndex));
		}
	}
}

void FFolderAliasCustomization::TryInstallFolderAliasToolTip(const TSharedRef<SWidget>& InWidget)
{
	if (!IsAssetViewItemTypeName(InWidget->GetType()))
	{
		return;
	}

	for (const TWeakPtr<SWidget>& Handled : HandledFolderItemWidgets)
	{
		if (Handled.Pin().Get() == &InWidget.Get())
		{
			return;
		}
	}

	SAssetViewItem* ItemWidget = static_cast<SAssetViewItem*>(&InWidget.Get());
	const TSharedPtr<FAssetViewItem> AssetItem = ItemWidget->*FAssetViewItemAccess::AssetItemPtr();
	if (!AssetItem.IsValid())
	{
		return;
	}

	// 用已导出的 GetItem() + IsFolder() 判断是否为文件夹
	if (!AssetItem->GetItem().IsFolder())
	{
		return;
	}

	InWidget->SetToolTip(SNew(FFolderAliasItemToolTip).TargetItemWidget(InWidget));
	HandledFolderItemWidgets.Add(InWidget);
}

#undef LOCTEXT_NAMESPACE
