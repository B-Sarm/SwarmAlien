// Copyright (c) 2006-2017 Audiokinetic Inc. / All Rights Reserved

/*------------------------------------------------------------------------------------
	SWaapiPicker.cpp
------------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------------
 includes.
------------------------------------------------------------------------------------*/
#include "WaapiPicker/SWaapiPicker.h"
#include "WaapiPicker/SWaapiPickerRow.h"
#include "WaapiPicker/WaapiPickerViewCommands.h"
#include "AkWaapiUtils.h"
#include "AkAudioStyle.h"
#include "AkSettings.h"
#include "AkAudioDevice.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"

/*------------------------------------------------------------------------------------
Defines
------------------------------------------------------------------------------------*/
#define LOCTEXT_NAMESPACE "AkAudio"

DEFINE_LOG_CATEGORY(LogAkAudioPicker);

DECLARE_CYCLE_STAT(TEXT("WaapiPicker - ConstructTree"), STAT_WaapiPickerConstructTree, STATGROUP_Audio);
DECLARE_CYCLE_STAT(TEXT("WaapiPicker - TreeExpansionChanged"), STAT_WaapiPickerTreeExpansionChanged, STATGROUP_Audio);

/*------------------------------------------------------------------------------------
Statics and Globals
------------------------------------------------------------------------------------*/

const FName SWaapiPicker::WaapiPickerTabName = FName("WaapiPicker");

static inline void CallWaapiGetProjectNamePath(FString& ProjectName, FString& ProjectPath)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return;

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	{
		TSharedPtr<FJsonObject> OfType = MakeShared<FJsonObject>();
		OfType->SetArrayField(WwiseWaapiHelper::OF_TYPE, TArray<TSharedPtr<FJsonValue>> { MakeShared<FJsonValueString>(WwiseWaapiHelper::PROJECT) });
		args->SetObjectField(WwiseWaapiHelper::FROM, OfType);
	}

	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	{
		options->SetArrayField(WwiseWaapiHelper::RETURN, TArray<TSharedPtr<FJsonValue>>
		{
			MakeShared<FJsonValueString>(WwiseWaapiHelper::NAME),
			MakeShared<FJsonValueString>(WwiseWaapiHelper::FILEPATH),
		});
	}
	
	TSharedPtr<FJsonObject> outJsonResult;
	if (waapiClient->Call(ak::wwise::core::object::get, args, options, outJsonResult))
	{
		// Recover the information from the Json object getResult and use it to get the item id.
		TArray<TSharedPtr<FJsonValue>> StructJsonArray = outJsonResult->GetArrayField(WwiseWaapiHelper::RETURN);
		if (StructJsonArray.Num())
		{
			auto Path = StructJsonArray[0]->AsObject()->GetStringField(WwiseWaapiHelper::FILEPATH);
			ProjectPath = FPaths::GetPath(Path);
			ProjectName = FPaths::GetCleanFilename(Path);
		}
		else
		{
			UE_LOG(LogAkAudioPicker, Log, TEXT("Unable to get the project name"));
		}
	}
}

inline TSharedPtr<FWwiseTreeItem> SWaapiPicker::FindItemFromPath(const TSharedPtr<FWwiseTreeItem>& ParentItem,const FString& CurrentItemPath)
{
	// We get the element to create in an array and loop over it to create them.
	TArray<FString> itemPathArray;
	CurrentItemPath.ParseIntoArray(itemPathArray, *WwiseWaapiHelper::BACK_SLASH);
	TSharedPtr<FWwiseTreeItem> PreviousItem = ParentItem;
	for (int i = 1; i < itemPathArray.Num(); i++)
	{
		TSharedPtr<FWwiseTreeItem> ChildItem = PreviousItem->GetChild(itemPathArray[i]);
		if (!ChildItem.IsValid())
		{
			return TSharedPtr<FWwiseTreeItem>(NULL);
		}
		PreviousItem = ChildItem;
	}
	return PreviousItem;
}

inline void SWaapiPicker::FindAndCreateItems(TSharedPtr<FWwiseTreeItem> CurrentItem)
{
	LastExpandedItems.Add(CurrentItem->ItemId);
	FString LastPathVisited = CurrentItem->FolderPath;
	LastPathVisited.RemoveFromEnd(WwiseWaapiHelper::BACK_SLASH + CurrentItem->DisplayName);
	TSharedPtr<FWwiseTreeItem> RootItem = GetRootItem(CurrentItem->FolderPath);
	if (CurrentItem->FolderPath == RootItem->FolderPath)
	{
		return;
	}
	else if (LastPathVisited == RootItem->FolderPath)
	{
		CurrentItem->Parent = RootItem->Parent.Pin();
		RootItem->Children.Add(CurrentItem);		
		return;
	}
	TSharedPtr<FWwiseTreeItem> ParentItem = FindItemFromPath(RootItem, LastPathVisited);
	if (ParentItem.IsValid())
	{
		CurrentItem->Parent = ParentItem->Parent.Pin();
		ParentItem->Children.Add(CurrentItem);
	}
	else
	{
		TSharedPtr<FJsonObject> getResult;
		// Request data from Wwise UI using WAAPI and use them to create a Wwise tree item, getting the informations from a specific "PATH".
		if (CallWaapiGetInfoFrom(WwiseWaapiHelper::PATH, LastPathVisited, getResult, {}))
		{
			// Recover the information from the Json object getResult and use it to construct the tree item.
			TSharedPtr<FWwiseTreeItem> NewRootItem = ConstructWwiseTreeItem(getResult->GetArrayField(WwiseWaapiHelper::RETURN)[0]);
			CurrentItem->Parent = NewRootItem;
			NewRootItem->Children.Add(CurrentItem);
			FindAndCreateItems(NewRootItem);
		}
		else
		{
			UE_LOG(LogAkAudioPicker, Log, TEXT("Failed to get information from path : %s"), *LastPathVisited);
		}
	}
}

inline TSharedPtr<FWwiseTreeItem> SWaapiPicker::GetRootItem(const FString& InFullPath)
{
	for (int i = EWwiseItemType::Event; i <= EWwiseItemType::LastWaapiDraggable; ++i)
	{
		if (InFullPath.StartsWith(RootItems[i]->FolderPath))
		{
			return RootItems[i];
		}
	}

	return {};
}

bool SWaapiPicker::CallWaapiGetInfoFrom(const FString& inFromField, const FString& inFromString, TSharedPtr<FJsonObject>& outJsonResult, const TArray<TransformStringField>& TransformFields)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return false;

	// Construct the arguments Json object : Getting infos "from - a specific id/path"
	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	{
		TSharedPtr<FJsonObject> from = MakeShared<FJsonObject>();
		from->SetArrayField(inFromField, TArray<TSharedPtr<FJsonValue>> { MakeShared<FJsonValueString>(inFromString) });
		args->SetObjectField(WwiseWaapiHelper::FROM, from);

		// In case we would recover the children of the object that have the id : ID or the path : PATH, then we set isGetChildren to true.

		if (TransformFields.Num())
		{		
			TArray<TSharedPtr<FJsonValue>> transform;
			
			for (auto TransformValue : TransformFields)
			{
				TSharedPtr<FJsonObject> insideTransform = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> JsonArray;
				for (auto TransformStringValueArg : TransformValue.valueStringArgs)
				{
					JsonArray.Add(MakeShared<FJsonValueString>(TransformStringValueArg));
				}
				for (auto TransformNumberValueArg : TransformValue.valueNumberArgs)
				{
					JsonArray.Add(MakeShared<FJsonValueNumber>(TransformNumberValueArg));
				}
				insideTransform->SetArrayField(TransformValue.keyArg, JsonArray);
				transform.Add(MakeShared<FJsonValueObject>(insideTransform));
			}
			args->SetArrayField(WwiseWaapiHelper::TRANSFORM, transform);
		}
	}

	// Construct the options Json object : Getting specific infos to construct the wwise tree item "id - name - type - childrenCount - path - parent"
	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	options->SetArrayField(WwiseWaapiHelper::RETURN, TArray<TSharedPtr<FJsonValue>>
	{
		MakeShared<FJsonValueString>(WwiseWaapiHelper::ID),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::NAME),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::TYPE),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::CHILDREN_COUNT),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::PATH),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::WORKUNIT_TYPE),
	});

	// Request data from Wwise using WAAPI

	return waapiClient->Call(ak::wwise::core::object::get, args, options, outJsonResult);
}

TSharedPtr<FWwiseTreeItem> SWaapiPicker::ConstructWwiseTreeItem(const TSharedPtr<FJsonValue>& InJsonItem)
{
	return ConstructWwiseTreeItem(InJsonItem->AsObject());
}

TSharedPtr<FWwiseTreeItem> SWaapiPicker::ConstructWwiseTreeItem(const TSharedPtr<FJsonObject>& ItemInfoObj)
{
	static const FString ValidPaths[] = {
		EWwiseItemType::FolderNames[EWwiseItemType::Event],
		EWwiseItemType::FolderNames[EWwiseItemType::AuxBus],
		EWwiseItemType::FolderNames[EWwiseItemType::ActorMixer],
		EWwiseItemType::FolderNames[EWwiseItemType::GameParameter],
		EWwiseItemType::FolderNames[EWwiseItemType::State],
		EWwiseItemType::FolderNames[EWwiseItemType::Switch],
		EWwiseItemType::FolderNames[EWwiseItemType::Trigger],
		EWwiseItemType::FolderNames[EWwiseItemType::AcousticTexture]
	};

	static auto isValidPath = [](const FString& input, const auto& source) -> bool {
		for (const auto& item : source)
		{
			if (input.StartsWith(WwiseWaapiHelper::BACK_SLASH + item))
			{
				return true;
			}
		}

		return false;
	};

	const FString itemTypeString = ItemInfoObj->GetStringField(WwiseWaapiHelper::TYPE);

	auto itemType = EWwiseItemType::FromString(itemTypeString);
	if (itemType == EWwiseItemType::None)
	{
		return {};
	}

	const FString itemPath = ItemInfoObj->GetStringField(WwiseWaapiHelper::PATH);
	if (isValidPath(itemPath, ValidPaths))
	{
		const FString itemIdString = ItemInfoObj->GetStringField(WwiseWaapiHelper::ID);
		FGuid in_ItemId = FGuid::NewGuid();
		FGuid::ParseExact(itemIdString, EGuidFormats::DigitsWithHyphensInBraces, in_ItemId);
		const FString itemName = ItemInfoObj->GetStringField(WwiseWaapiHelper::NAME);

		if (itemName.IsEmpty())
		{
			return {};
		}

		const uint32_t itemChilrenCount = ItemInfoObj->GetNumberField(WwiseWaapiHelper::CHILDREN_COUNT);

		if (itemType == EWwiseItemType::StandaloneWorkUnit)
		{
			FString WorkUnitType;
			if (ItemInfoObj->TryGetStringField(WwiseWaapiHelper::WORKUNIT_TYPE, WorkUnitType) && WorkUnitType == "FOLDER")
			{
				itemType = EWwiseItemType::PhysicalFolder;
			}
		}
		TSharedPtr<FWwiseTreeItem> treeItem = MakeShared<FWwiseTreeItem>(itemName, itemPath, nullptr, itemType, in_ItemId);
		if ((itemType != EWwiseItemType::Event) && (itemType != EWwiseItemType::Sound))
		{
			treeItem->ChildCountInWwise = itemChilrenCount;
		}
		return treeItem;
	}

	return {};
}

/*------------------------------------------------------------------------------------
Implementation
------------------------------------------------------------------------------------*/
SWaapiPicker::SWaapiPicker() : CommandList(MakeShared<FUICommandList>())
{
	AllowTreeViewDelegates = true;
	isPickerVisible = FAkWaapiClient::IsProjectLoaded();
}

void SWaapiPicker::RemoveClientCallbacks()
{
	auto waapiClient = FAkWaapiClient::Get();
	if (waapiClient == nullptr)
		return;

	if (ProjectLoadedHandle.IsValid())
	{
		waapiClient->OnProjectLoaded.Remove(ProjectLoadedHandle);
		ProjectLoadedHandle.Reset();
	}

	if (ConnectionLostHandle.IsValid())
	{
		waapiClient->OnConnectionLost.Remove(ConnectionLostHandle);
		ConnectionLostHandle.Reset();
	}

	UnsubscribeWaapiCallbacks();
}

SWaapiPicker::~SWaapiPicker()
{
	RootItems.Empty();

	RemoveClientCallbacks();

	if (auto waapiClient = FAkWaapiClient::Get())
	{
		waapiClient->OnClientBeginDestroy.Remove(ClientBeginDestroyHandle);
	}

	StopAndDestroyAllTransports();
}

void SWaapiPicker::Construct(const FArguments& InArgs)
{
	OnDragDetected = InArgs._OnDragDetected;
	OnSelectionChanged = InArgs._OnSelectionChanged;
	OnGenerateSoundBanksClicked = InArgs._OnGenerateSoundBanksClicked;

	CallWaapiGetProjectNamePath(ProjectName, ProjectFolder);
	bRestrictContextMenu = InArgs._RestrictContextMenu;

	if (InArgs._FocusSearchBoxWhenOpened)
	{
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SWaapiPicker::SetFocusPostConstruct));
	}
	FGenericCommands::Register();
	FWaapiPickerViewCommands::Register();
	CreateWaapiPickerCommands();

	SearchBoxFilter = MakeShared<StringFilter>(StringFilter::FItemToStringArray::CreateSP(this, &SWaapiPicker::PopulateSearchStrings));
	SearchBoxFilter->OnChanged().AddSP(this, &SWaapiPicker::FilterUpdated);

	if (auto* settings = GetMutableDefault<UAkSettings>())
	{
		settings->bRequestRefresh = false;
	}
	
	ChildSlot
	[
		SNew(SBorder)
		.Padding(4)
		.BorderImage(FAkAudioStyle::GetBrush("AudiokineticTools.GroupBorder"))
		[
			SNew(SOverlay)

			// Picker
			+ SOverlay::Slot()
			.VAlign(VAlign_Fill)
			[
				SNew(SVerticalBox)
				.Visibility(this, &SWaapiPicker::isPickerAllowed)

				// Search
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 1, 0, 3)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						InArgs._SearchContent.Widget
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SAssignNew(SearchBoxPtr,SSearchBox)
						.HintText( LOCTEXT( "WaapiPickerSearchHint", "Search Wwise Item" ) )
						.ToolTipText(LOCTEXT("WaapiPickerSearchTooltip", "Type here to search for a Wwise asset"))
						.OnTextChanged( this, &SWaapiPicker::OnSearchBoxChanged )
						.SelectAllTextWhenFocused(false)
						.DelayChangeNotificationsWhileTyping(true)
					]
				]

				// Tree title
				+SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(3.0f)
					[
						SNew(SImage) 
						.Image(FAkAudioStyle::GetBrush(EWwiseItemType::Project))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0,0,3,0)
					[
						SNew(STextBlock)
						.Font(FAkAudioStyle::GetFontStyle("AudiokineticTools.SourceTitleFont") )
						.Text( this, &SWaapiPicker::GetProjectName )
						.Visibility(InArgs._ShowTreeTitle ? EVisibility::Visible : EVisibility::Collapsed)
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1)
					[
						SNew( SSpacer )
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("AkPickerPopulate", "Populate"))
						.OnClicked(this, &SWaapiPicker::OnPopulateClicked)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("AkPickerGenerateSoundData", "Generate Sound Data..."))
						.OnClicked(this, &SWaapiPicker::OnGenerateSoundBanksButtonClicked)
						.Visibility(InArgs._ShowGenerateSoundBanksButton ? EVisibility::Visible : EVisibility::Collapsed)
					]
				]

				// Separator
				+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 1)
				[
					SNew(SSeparator)
					.Visibility( ( InArgs._ShowSeparator) ? EVisibility::Visible : EVisibility::Collapsed )
				]
				
				// Tree
				+SVerticalBox::Slot()
				.FillHeight(1.f)
				[
					SAssignNew(TreeViewPtr, STreeView< TSharedPtr<FWwiseTreeItem> >)
					.TreeItemsSource(&RootItems)
					.OnGenerateRow( this, &SWaapiPicker::GenerateRow )
					//.OnItemScrolledIntoView( this, &SPathView::TreeItemScrolledIntoView )
					.ItemHeight(18)
					.SelectionMode(InArgs._SelectionMode)
					.OnSelectionChanged(this, &SWaapiPicker::TreeSelectionChanged)
					.OnExpansionChanged(this, &SWaapiPicker::TreeExpansionChanged)
					.OnGetChildren( this, &SWaapiPicker::GetChildrenForTree )
					//.OnSetExpansionRecursive( this, &SPathView::SetTreeItemExpansionRecursive )
					.OnContextMenuOpening(this, &SWaapiPicker::MakeWaapiPickerContextMenu)
					.ClearSelectionOnClick(false)
				]
			]

			// Empty Picker
			+ SOverlay::Slot()
			.VAlign(VAlign_Center)
			.HAlign(HAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility(this, &SWaapiPicker::isWarningVisible)
					.AutoWrapText(true)
					.Justification(ETextJustify::Center)
					.Text(LOCTEXT("EmptyWaapiTree", "Could not establish a WAAPI connection; WAAPI picker is disabled. Please enable WAAPI in your Wwise settings, or use the Wwise Picker."))
				]
				+ SVerticalBox::Slot()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				.AutoHeight()
				[
					SNew(SHyperlink)
					.Visibility(this, &SWaapiPicker::isWarningVisible)
					.Text(LOCTEXT("WaapiDucumentation", "For more informaton, please Visit Waapi Documentation."))
					.ToolTipText(LOCTEXT("WaapiDucumentationTooltip", "Opens Waapi documentation in a new browser window"))
					.OnNavigate_Lambda([] { FPlatformProcess::LaunchURL(*FString("https://www.audiokinetic.com/library/?source=SDK&id=waapi.html"), nullptr, nullptr); })
				]
			]
		]
	];
	OnPopulateClicked();
	ExpandFirstLevel();

	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return;

	ProjectLoadedHandle = waapiClient->OnProjectLoaded.AddLambda([this]
	{
		/* Construct the tree when we have the same project */
		isPickerVisible = true;
		SubscribeWaapiCallbacks();
		CallWaapiGetProjectNamePath(ProjectName, ProjectFolder);
		ConstructTree();
	});

	ConnectionLostHandle = waapiClient->OnConnectionLost.AddLambda([this]
	{
		/* Empty the tree when we have different projects */
		isPickerVisible = false;
		UnsubscribeWaapiCallbacks();
		ConstructTree();
	});

	ClientBeginDestroyHandle = waapiClient->OnClientBeginDestroy.AddSP(this, &SWaapiPicker::RemoveClientCallbacks);
}

EVisibility SWaapiPicker::isPickerAllowed() const
{
	return isPickerVisible ? EVisibility::Visible : EVisibility::Hidden;
}

EVisibility SWaapiPicker::isWarningVisible() const
{
	return isPickerVisible ? EVisibility::Hidden : EVisibility::Visible;
}

void SWaapiPicker::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	auto AkSettings = GetMutableDefault<UAkSettings>();
	if(AkSettings->bRequestRefresh)
	{
		ConstructTree();
		AkSettings->bRequestRefresh = false;
	}
}

FText SWaapiPicker::GetProjectName() const
{
	return FText::FromString(ProjectName);
}

FReply SWaapiPicker::OnPopulateClicked()
{
	ConstructTree();
	return FReply::Handled();
}

FReply SWaapiPicker::OnGenerateSoundBanksButtonClicked()
{
	OnGenerateSoundBanksClicked.ExecuteIfBound();
	return FReply::Handled();
}

void SWaapiPicker::ConstructTree()
{
	if (FAkWaapiClient::IsProjectLoaded())
	{
		if (ConstructTreeTask.IsValid() && !ConstructTreeTask->IsComplete())
		{
			if (auto AkSettings = GetMutableDefault<UAkSettings>())
			{
				AkSettings->bRequestRefresh = true;
			}

			return;
		}

		FString CurrentFilterText = SearchBoxFilter.IsValid() ? SearchBoxFilter->GetRawFilterText().ToString() : TEXT("");
		if (!CurrentFilterText.IsEmpty())
		{
			FilterUpdated();
			return;
		}
		ConstructTreeTask = FFunctionGraphTask::CreateAndDispatchWhenReady([sharedThis = SharedThis(this)] {
			{
				FScopeLock autoLock(&sharedThis->RootItemsLock);
				sharedThis->RootItems.Empty(EWwiseItemType::LastWaapiDraggable - EWwiseItemType::Event + 1);
			}

			auto populateTask = FFunctionGraphTask::CreateAndDispatchWhenReady([sharedThis] {
				for (int i = EWwiseItemType::Event; i <= EWwiseItemType::LastWaapiDraggable; ++i)
				{
					FGuid in_ItemId = FGuid::NewGuid();
					TSharedPtr<FJsonObject> getResult;
					uint32_t itemChilrenCount = 0;
					FString path = WwiseWaapiHelper::BACK_SLASH + EWwiseItemType::FolderNames[i];
					// Request data from Wwise UI using WAAPI and use them to create a Wwise tree item, getting the informations from a specific "PATH".
					if (sharedThis->CallWaapiGetInfoFrom(WwiseWaapiHelper::PATH, path, getResult, {}))
					{
						// Recover the information from the Json object getResult and use it to get the item id.
						const TSharedPtr<FJsonObject>& ItemInfoObj = getResult->GetArrayField(WwiseWaapiHelper::RETURN)[0]->AsObject();
						const FString itemIdString = ItemInfoObj->GetStringField(WwiseWaapiHelper::ID);
						path = ItemInfoObj->GetStringField(WwiseWaapiHelper::PATH);
						itemChilrenCount = ItemInfoObj->GetNumberField(WwiseWaapiHelper::CHILDREN_COUNT);
						FGuid::ParseExact(itemIdString, EGuidFormats::DigitsWithHyphensInBraces, in_ItemId);
					}
					else
					{
						UE_LOG(LogAkAudioPicker, Log, TEXT("Failed to get information from id : %s"), *path);
						if (auto AkSettings = GetMutableDefault<UAkSettings>())
						{
							AkSettings->bRequestRefresh = true;
						}
						return;
					}
					// Create a new tree item and add it the root list.
					TSharedPtr<FWwiseTreeItem> NewRootParent = MakeShared<FWwiseTreeItem>(EWwiseItemType::ItemNames[i], path, nullptr, EWwiseItemType::PhysicalFolder, in_ItemId);
					NewRootParent->ChildCountInWwise = itemChilrenCount;

					{
						FScopeLock autoLock(&sharedThis->RootItemsLock);
						sharedThis->RootItems.Add(NewRootParent);
					}
				}
			}, GET_STATID(STAT_WaapiPickerConstructTree), nullptr, ENamedThreads::AnyThread);

			FTaskGraphInterface::Get().WaitUntilTaskCompletes(populateTask);

			FFunctionGraphTask::CreateAndDispatchWhenReady([sharedThis] {
				sharedThis->AllowTreeViewDelegates = true;

				sharedThis->ExpandFirstLevel();
				sharedThis->RestoreTreeExpansion(sharedThis->RootItems);

				sharedThis->TreeViewPtr->RequestTreeRefresh();
			}, GET_STATID(STAT_WaapiPickerConstructTree), nullptr, ENamedThreads::GameThread);

		}, GET_STATID(STAT_WaapiPickerConstructTree), nullptr, ENamedThreads::AnyThread);
	}
}

void SWaapiPicker::ExpandFirstLevel()
{
	// Expand root items and first-level work units.
	for(int32 i = 0; i < RootItems.Num(); i++)
	{
		TreeViewPtr->SetItemExpansion(RootItems[i], true);
	}
}

void SWaapiPicker::ExpandParents(TSharedPtr<FWwiseTreeItem> Item)
{
	if(Item->Parent.IsValid())
	{
		ExpandParents(Item->Parent.Pin());
		TreeViewPtr->SetItemExpansion(Item->Parent.Pin(), true);
	}
}

TSharedRef<ITableRow> SWaapiPicker::GenerateRow( TSharedPtr<FWwiseTreeItem> TreeItem, const TSharedRef<STableViewBase>& OwnerTable )
{
	check(TreeItem.IsValid());
	
	EVisibility RowVisibility = TreeItem->IsVisible ? EVisibility::Visible : EVisibility::Collapsed;
	
	TSharedPtr<ITableRow> NewRow = SNew(STableRow< TSharedPtr<FWwiseTreeItem> >, OwnerTable)
		.OnDragDetected(this, &SWaapiPicker::HandleOnDragDetected)
		.Visibility(RowVisibility)
		[
			SNew(SWaapiPickerRow)
			.WaapiPickerItem(TreeItem)
			.HighlightText(this, &SWaapiPicker::GetHighlightText)
			.IsSelected(this, &SWaapiPicker::IsTreeItemSelected, TreeItem)
		];
	
	TreeItem->TreeRow = NewRow;
	
	return NewRow.ToSharedRef();
}

void SWaapiPicker::GetChildrenForTree( TSharedPtr< FWwiseTreeItem > TreeItem, TArray< TSharedPtr<FWwiseTreeItem> >& OutChildren )
{
	// In case the item is "unexpanded" and have children (in the Wwise tree), we need to add a default item to show the arrow that says, this item have children.
	FString CurrentFilterText = SearchBoxFilter->GetRawFilterText().ToString();

	if (!TreeItem->ChildCountInWwise)
	{
		// This is useful in case when the item contains elements that are being moved to an other path and the item
		// has no longer children and was expanded, so we need to remove it form the expansion items list.
		LastExpandedItems.Remove(TreeItem->ItemId);
	}
	else if (CurrentFilterText.IsEmpty())
	{
		if (!LastExpandedItems.Contains(TreeItem->ItemId))
		{
			TreeItem->Children.Empty();
			TSharedPtr<FWwiseTreeItem> emptyTreeItem = MakeShared<FWwiseTreeItem>(WwiseWaapiHelper::NAME, WwiseWaapiHelper::PATH, nullptr, EWwiseItemType::PhysicalFolder, FGuid::NewGuid());
			TreeItem->Children.Add(emptyTreeItem);
		}
		else
		{
			// Update the item expansion to be visible in the tree, since it is being expanded by the user.
			TreeViewPtr->SetItemExpansion(TreeItem, true);
		}
	}

	OutChildren = TreeItem->Children;
}

FReply SWaapiPicker::HandleOnDragDetected(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	// Refresh the contents
	if(OnDragDetected.IsBound())
		return OnDragDetected.Execute(Geometry,MouseEvent);

	return FReply::Unhandled();
}

void SWaapiPicker::PopulateSearchStrings(const FString& FolderName, OUT TArray< FString >& OutSearchStrings) const
{
	OutSearchStrings.Add(FolderName);
}

void SWaapiPicker::OnSearchBoxChanged(const FText& InSearchText)
{
	SearchBoxFilter->SetRawFilterText(InSearchText);
}

FText SWaapiPicker::GetHighlightText() const
{
	return SearchBoxFilter->GetRawFilterText();
}

void SWaapiPicker::FilterUpdated()
{
	FScopedSlowTask SlowTask(2.f, LOCTEXT("AK_PopulatingPicker", "Populating Waapi Picker..."));
	SlowTask.MakeDialog();
	if (RootItems.Num())
	{
		ApplyFilter();
	}
	TreeViewPtr->RequestTreeRefresh();
}

void SWaapiPicker::SetItemVisibility(TSharedPtr<FWwiseTreeItem> Item, bool IsVisible)
{
	if (!Item.IsValid())
		return;

	if (IsVisible)
	{
		// Propagate visibility to parents.
		SetItemVisibility(Item->Parent.Pin(), IsVisible);
	}
	Item->IsVisible = IsVisible;
	if (Item->TreeRow.IsValid())
	{
		TSharedRef<SWidget> wid = Item->TreeRow.Pin()->AsWidget();
		wid->SetVisibility(IsVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

void SWaapiPicker::ApplyFilter()
{
	for (int i = EWwiseItemType::Event; i <= EWwiseItemType::LastWaapiDraggable; ++i)
	{
		RootItems[i]->Children.Empty();
	}

	static TSet<FGuid> LastExpandedItemsBeforFilter;
	AllowTreeViewDelegates = false;
	FString CurrentFilterText = SearchBoxFilter->GetRawFilterText().ToString();
	if (CurrentFilterText.IsEmpty())
	{
		// Recover the last expanded items before filtering.
		LastExpandedItems.Empty();
		LastExpandedItems = LastExpandedItemsBeforFilter;
		LastExpandedItemsBeforFilter.Empty();
		AllowTreeViewDelegates = true;
		OnPopulateClicked();
		return;
	}

	if (!LastExpandedItemsBeforFilter.Num())
	{
		// We preserve the last expanded items to re-expand the tree as it was in non filtering mode.
		LastExpandedItemsBeforFilter = LastExpandedItems;
		LastExpandedItems.Empty();
	}

	TSharedPtr<FJsonObject> getResult;
	if (CallWaapiGetInfoFrom(WwiseWaapiHelper::SEARCH, CurrentFilterText, getResult, { { WwiseWaapiHelper::WHERE, { WwiseWaapiHelper::NAMECONTAINS, CurrentFilterText }, {} }, { WwiseWaapiHelper::RANGE, {}, { 0, 2000*CurrentFilterText.Len() } } }))
	{
		// Recover the information from the Json object getResult and use it to construct the tree item.
		TArray<TSharedPtr<FJsonValue>> SearchResultArray = getResult->GetArrayField(WwiseWaapiHelper::RETURN);
		if (SearchResultArray.Num())
		{
			// The map contains each path and the correspondent object of the search result.
			TMap < FString, TSharedPtr<FWwiseTreeItem>> SearchedResultTreeItem;
			for (int i = 0; i < SearchResultArray.Num(); i++)
			{
				// Fill the map with the path-object elements.
				TSharedPtr<FWwiseTreeItem> NewRootChild = ConstructWwiseTreeItem(SearchResultArray[i]);
				if (NewRootChild.IsValid())
				{
					FindAndCreateItems(NewRootChild);
				}
			}
		}
	}
	else
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Failed to get information from item search : %s"), *CurrentFilterText);
	}

	RestoreTreeExpansion(RootItems);
	AllowTreeViewDelegates = true;
}

void SWaapiPicker::RestoreTreeExpansion(const TArray< TSharedPtr<FWwiseTreeItem> >& Items)
{
	for(int i = 0; i < Items.Num(); i++)
	{
		if(LastExpandedItems.Contains(Items[i]->ItemId) )
		{
			TreeViewPtr->SetItemExpansion(Items[i], true);
		}
		RestoreTreeExpansion(Items[i]->Children);
	}
}

void SWaapiPicker::TreeSelectionChanged(TSharedPtr< FWwiseTreeItem > TreeItem, ESelectInfo::Type /*SelectInfo*/)
{
	if (AllowTreeViewDelegates)
	{
		auto& SelectedItems = GetSelectedItems();

		LastSelectedItems.Empty();
		for (int32 ItemIdx = 0; ItemIdx < SelectedItems.Num(); ++ItemIdx)
		{
			auto& Item = SelectedItems[ItemIdx];
			if (Item.IsValid())
			{
				LastSelectedItems.Add(Item->ItemId);
			}
		}

		OnSelectionChanged.ExecuteIfBound(TreeItem, ESelectInfo::OnMouseClick);
	}
}

void SWaapiPicker::TreeExpansionChanged(TSharedPtr< FWwiseTreeItem > TreeItem, bool bIsExpanded)
{
	if (!AllowTreeViewDelegates)
	{
		if (bIsExpanded)
			TreeItem->SortChildren();

		return;
	}

	// If the item is not expanded we don't need to request the server to get any information(the children are hidden).
	if (!bIsExpanded)
	{
		LastExpandedItems.Remove(TreeItem->ItemId);
		return;
	}

	LastExpandedItems.Add(TreeItem->ItemId);

	FString CurrentFilterText = SearchBoxFilter->GetRawFilterText().ToString();
	if (!CurrentFilterText.IsEmpty())
		return;

	const FString itemIdStringField = TreeItem->ItemId.ToString(EGuidFormats::DigitsWithHyphensInBraces);
	
	FFunctionGraphTask::CreateAndDispatchWhenReady([sharedThis = SharedThis(this), TreeItem, itemIdStringField] {
		TSharedPtr<FJsonObject> result;
		// Request data from Wwise UI using WAAPI and use them to create a Wwise tree item, getting the informations from a specific "ID".
		if (!sharedThis->CallWaapiGetInfoFrom(WwiseWaapiHelper::ID, itemIdStringField, result, { { WwiseWaapiHelper::SELECT, { WwiseWaapiHelper::CHILDREN }, {} } }))
		{
			UE_LOG(LogAkAudioPicker, Log, TEXT("Failed to get information from id : %s"), *itemIdStringField);
			return;
		}

		FFunctionGraphTask::CreateAndDispatchWhenReady([sharedThis, result, TreeItem] {
			// The tree view might have been destroyed between scheduling and running this task
			// Recover the information from the Json object getResult and use it to construct the tree item.
			TArray<TSharedPtr<FJsonValue>> StructJsonArray = result->GetArrayField(WwiseWaapiHelper::RETURN);
			/** If the item have just one child and we are expanding it, this means that we need to construct the children list.
			*   In case the the number of children gotten form Wwise is not the same of the item, this means that there is some children added/removed,
			*   so we also need to construct the new list.
			*/
			if ((TreeItem->Children.Num() == 1) || (TreeItem->Children.Num() != StructJsonArray.Num()))
			{
				TreeItem->Children.Empty();
				for (int i = 0; i < StructJsonArray.Num(); i++)
				{
					TSharedPtr<FWwiseTreeItem> NewRootChild = sharedThis->ConstructWwiseTreeItem(StructJsonArray[i]);
					if (NewRootChild.IsValid())
					{
						TreeItem->Children.Add(NewRootChild);
						NewRootChild->Parent = TreeItem;
					}
				}

				TreeItem->SortChildren();

				sharedThis->TreeViewPtr->RequestTreeRefresh();
			}
		}, GET_STATID(STAT_WaapiPickerTreeExpansionChanged), nullptr, ENamedThreads::GameThread);

	}, GET_STATID(STAT_WaapiPickerTreeExpansionChanged));
}

bool SWaapiPicker::IsTreeItemSelected(TSharedPtr<FWwiseTreeItem> TreeItem) const
{
	return TreeViewPtr->IsItemSelected(TreeItem);
}

TSharedPtr<SWidget> SWaapiPicker::MakeWaapiPickerContextMenu()
{
	const FWaapiPickerViewCommands& Commands = FWaapiPickerViewCommands::Get();

	// Build up the menu
	FMenuBuilder MenuBuilder(true, CommandList);
	{
		MenuBuilder.BeginSection("WaapiPickerCreate", LOCTEXT("MenuHeader", "WaapiPicker"));
		{
			MenuBuilder.AddMenuEntry(Commands.RequestPlayWwiseItem);
			MenuBuilder.AddMenuEntry(Commands.RequestStopAllWwiseItem);
		}
		MenuBuilder.EndSection();
		MenuBuilder.BeginSection("WaapiPickerEdit", LOCTEXT("EditMenuHeader", "Edit"));
		{
			MenuBuilder.AddMenuEntry(Commands.RequestRenameWwiseItem);
			MenuBuilder.AddMenuEntry(Commands.RequestDeleteWwiseItem);
		}
		MenuBuilder.EndSection();
		if (!bRestrictContextMenu)
		{
			MenuBuilder.BeginSection("WaapiPickerExplore", LOCTEXT("ExploreMenuHeader", "Explore"));
			{
				MenuBuilder.AddMenuEntry(Commands.RequestExploreWwiseItem);
				MenuBuilder.AddMenuEntry(Commands.RequestFindInProjectExplorerWwisetem);
			}
			MenuBuilder.EndSection();
		}
		MenuBuilder.BeginSection("WaapiPickerRefreshAll");
		{
			MenuBuilder.AddMenuEntry(Commands.RequestRefreshWaapiPicker);

		}
		MenuBuilder.EndSection();
	}
	return MenuBuilder.MakeWidget();
}

void SWaapiPicker::CreateWaapiPickerCommands()
{
	const FWaapiPickerViewCommands& Commands = FWaapiPickerViewCommands::Get();
	FUICommandList& ActionList = *CommandList;

	// Action for rename a Wwise item.
	ActionList.MapAction(Commands.RequestRenameWwiseItem,
		FExecuteAction::CreateRaw(this, &SWaapiPicker::HandleRenameWwiseItemCommandExecute),
		FCanExecuteAction::CreateRaw(this, &SWaapiPicker::HandleRenameWwiseItemCommandCanExecute));

	// Action to play a Wwise item (event).
	ActionList.MapAction(Commands.RequestPlayWwiseItem,
		FExecuteAction::CreateRaw(this, &SWaapiPicker::HandlePlayWwiseItemCommandExecute),
		FCanExecuteAction::CreateRaw(this, &SWaapiPicker::HandlePlayWwiseItemCommandCanExecute));

	// Action to stop all playing Wwise item (event).
	ActionList.MapAction(Commands.RequestStopAllWwiseItem,
		FExecuteAction::CreateSP(this, &SWaapiPicker::StopAndDestroyAllTransports));

	// Action for rename a Wwise item.
	ActionList.MapAction(Commands.RequestDeleteWwiseItem,
		FExecuteAction::CreateRaw(this, &SWaapiPicker::HandleDeleteWwiseItemCommandExecute),
		FCanExecuteAction::CreateRaw(this, &SWaapiPicker::HandleDeleteWwiseItemCommandCanExecute));

	// Explore an item in the containing folder.
	ActionList.MapAction(Commands.RequestExploreWwiseItem,
		FExecuteAction::CreateRaw(this, &SWaapiPicker::HandleExploreWwiseItemCommandExecute),
		FCanExecuteAction::CreateRaw(this, &SWaapiPicker::HandleWwiseCommandCanExecute));

	// Explore an item in the containing folder.
	ActionList.MapAction(Commands.RequestFindInProjectExplorerWwisetem,
		FExecuteAction::CreateRaw(this, &SWaapiPicker::HandleFindWwiseItemInProjectExplorerCommandExecute),
		FCanExecuteAction::CreateRaw(this, &SWaapiPicker::HandleWwiseCommandCanExecute));

	// Action for refresh the Waapi Picker.
	ActionList.MapAction(Commands.RequestRefreshWaapiPicker,
		FExecuteAction::CreateSP(this, &SWaapiPicker::HandleRefreshWaapiPickerCommandExecute));

	// Action for undo last action in the Waapi Picker.
	ActionList.MapAction(
		FGenericCommands::Get().Undo,
		FExecuteAction::CreateSP(this, &SWaapiPicker::HandleUndoWaapiPickerCommandExecute));

	// Action for redo last action in the Waapi Picker.
	ActionList.MapAction(
		FGenericCommands::Get().Redo,
		FExecuteAction::CreateSP(this, &SWaapiPicker::HandleRedoWaapiPickerCommandExecute));
}


bool SWaapiPicker::HandleRenameWwiseItemCommandCanExecute() const
{
	auto& SelectedItems = GetSelectedItems();
	return SelectedItems.Num() == 1 && SelectedItems[0]->IsNotOfType({ EWwiseItemType::PhysicalFolder, EWwiseItemType::StandaloneWorkUnit, EWwiseItemType::NestedWorkUnit });
}

void SWaapiPicker::HandleRenameWwiseItemCommandExecute() const
{
	auto& SelectedItems = GetSelectedItems();
	if (SelectedItems.Num())
	{
		TSharedPtr<ITableRow> TableRow = TreeViewPtr->WidgetFromItem(SelectedItems[0]);
		// If the Wwise item is selected but not visible, we scroll it into the view.
		if (!TableRow.IsValid())
		{
			TreeViewPtr->RequestScrollIntoView(SelectedItems[0]);
			return;
		}
		// Get the right Row to enter in editing mode.
		TSharedPtr<STableRow< TSharedPtr<FWwiseTreeItem>> > TableRowItem = StaticCastSharedPtr<STableRow< TSharedPtr<FWwiseTreeItem>>>(TableRow);
		if (TableRowItem.IsValid())
		{
			TSharedPtr<SWidget> RowContent = TableRowItem->GetContent();
			TSharedPtr<SWaapiPickerRow> ItemWidget = StaticCastSharedPtr<SWaapiPickerRow>(RowContent);
			if (ItemWidget.IsValid())
			{
				ItemWidget->EnterEditingMode();
			}
		}
	}
}

bool SWaapiPicker::HandlePlayWwiseItemCommandCanExecute() const
{
	auto& SelectedItems = GetSelectedItems();
	if (SelectedItems.Num() == 0)
		return false;

	for (int32 i = 0; i < SelectedItems.Num(); ++i)
	{
		if (SelectedItems[i]->IsNotOfType({ EWwiseItemType::Event, EWwiseItemType::Sound, EWwiseItemType::BlendContainer, EWwiseItemType::SwitchContainer, EWwiseItemType::RandomSequenceContainer }))
			return false;
	}

	return true;
}

int32 SWaapiPicker::CreateTransport(const FGuid& in_ItemId)
{
	const FString itemIdStringField = in_ItemId.ToString(EGuidFormats::DigitsWithHyphensInBraces);
	TSharedPtr<FJsonObject> getResult;
	int32 transportID = -1;
	if (SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::core::transport::create, { { WwiseWaapiHelper::OBJECT, itemIdStringField } }, getResult))
	{
		transportID = getResult->GetIntegerField(WwiseWaapiHelper::TRANSPORT);
		uint64 subscriptionID = SubscribeToTransportStateChanged(transportID);
		ItemToTransport.Add(in_ItemId, TransportInfo(transportID, subscriptionID));
	}

	return transportID;
}

void SWaapiPicker::DestroyTransport(const FGuid& in_itemID)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return;

	if(!ItemToTransport.Contains(in_itemID))
		return;

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	args->SetNumberField(WwiseWaapiHelper::TRANSPORT, ItemToTransport[in_itemID].TransportID);

	TSharedPtr<FJsonObject> getResult;
	if(ItemToTransport[in_itemID].SubscriptionID != 0)
		waapiClient->Unsubscribe(ItemToTransport[in_itemID].SubscriptionID, getResult);

	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	if (waapiClient->Call(ak::wwise::core::transport::destroy, args, options, getResult))
		ItemToTransport.Remove(in_itemID);
}

void SWaapiPicker::TogglePlayStop(int32 in_transportID)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Unable to connect to localhost"));
		return;
	}

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	args->SetStringField(WwiseWaapiHelper::ACTION, WwiseWaapiHelper::PLAYSTOP);
	args->SetNumberField(WwiseWaapiHelper::TRANSPORT, in_transportID);

	TSharedPtr<FJsonObject> getResult;
	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	if (!waapiClient->Call(ak::wwise::core::transport::executeAction, args, options, getResult))
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Failed to trigger playback"));
	}
}

void SWaapiPicker::StopTransport(int32 in_transportID)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return;

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	args->SetStringField(WwiseWaapiHelper::ACTION, WwiseWaapiHelper::STOP);
	args->SetNumberField(WwiseWaapiHelper::TRANSPORT, in_transportID);

	TSharedPtr<FJsonObject> getResult;
	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	if (!waapiClient->Call(ak::wwise::core::transport::executeAction, args, options, getResult))
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Cannot stop event."));
	}
}

void SWaapiPicker::HandleStateChanged(TSharedPtr<FJsonObject> in_UEJsonObject)
{
	const FString newState = in_UEJsonObject->GetStringField(WwiseWaapiHelper::STATE);
	FGuid itemID;
	FGuid::Parse(in_UEJsonObject->GetStringField(WwiseWaapiHelper::OBJECT), itemID);
	const int32 transportID = in_UEJsonObject->GetNumberField(WwiseWaapiHelper::TRANSPORT);
	if (newState == WwiseWaapiHelper::STOPPED)
	{
		DestroyTransport(itemID);
	}
	else if (newState == WwiseWaapiHelper::PLAYING && !ItemToTransport.Contains(itemID))
	{
		ItemToTransport.Add(itemID, TransportInfo(transportID, 0));
	}
}

uint64 SWaapiPicker::SubscribeToTransportStateChanged(int32 in_transportID)
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
		return 0;

	auto wampEventCallback = WampEventCallback::CreateLambda([&](uint64_t id, TSharedPtr<FJsonObject> in_UEJsonObject)
	{
		AsyncTask(ENamedThreads::GameThread, [this, in_UEJsonObject]
		{
			HandleStateChanged(in_UEJsonObject);
		});
	});

	TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetNumberField(WwiseWaapiHelper::TRANSPORT, in_transportID);

	TSharedPtr<FJsonObject> outJsonResult;
	uint64 subscriptionID = 0;
	waapiClient->Subscribe(ak::wwise::core::transport::stateChanged, Options, wampEventCallback, subscriptionID, outJsonResult);
	return subscriptionID;
}

void SWaapiPicker::HandlePlayWwiseItemCommandExecute()
{
	auto& SelectedItems = GetSelectedItems();

	// Loop to play all selected items.
	for (int32 i = 0; i < SelectedItems.Num(); ++i)
	{
		const FGuid& ItemId = SelectedItems[i]->ItemId;
		int32 transportID = -1;
		if (ItemToTransport.Contains(ItemId))
		{
			transportID = ItemToTransport[ItemId].TransportID;
		}
		else
		{
			transportID = CreateTransport(ItemId);
		}

		TogglePlayStop(transportID);
	}
}

void SWaapiPicker::StopAndDestroyAllTransports()
{
	for (auto iter = ItemToTransport.CreateIterator(); iter; ++iter)
	{
		StopTransport(iter->Value.TransportID);
		DestroyTransport(iter->Key);
	}
	ItemToTransport.Empty();
}

bool SWaapiPicker::HandleDeleteWwiseItemCommandCanExecute() const
{
	auto& SelectedItems = GetSelectedItems();
	if ((SelectedItems.Num() > 0) && 
		!(TreeViewPtr->IsItemSelected(RootItems[EWwiseItemType::Event]) || TreeViewPtr->IsItemSelected(RootItems[EWwiseItemType::AuxBus]) 
			|| TreeViewPtr->IsItemSelected(RootItems[EWwiseItemType::ActorMixer]) || TreeViewPtr->IsItemSelected(RootItems[EWwiseItemType::AcousticTexture])))
	{
		for (int32 i = 0; i < SelectedItems.Num(); ++i)
		{
			if (SelectedItems[i]->IsOfType({ EWwiseItemType::PhysicalFolder, EWwiseItemType::StandaloneWorkUnit, EWwiseItemType::NestedWorkUnit }))
				return false;
		}
		return true;
	}
	return false;
}

void SWaapiPicker::HandleDeleteWwiseItemCommandExecute()
{
	TSharedPtr<FJsonObject> getResult;
	SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::core::undo::beginGroup, {}, getResult);
	auto& SelectedItems = GetSelectedItems();
	for (int32 i = 0; i < SelectedItems.Num(); ++i)
	{
		const FString itemIdStringField = SelectedItems[i]->ItemId.ToString(EGuidFormats::DigitsWithHyphensInBraces);
		SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::core::object::delete_, { { WwiseWaapiHelper::OBJECT, itemIdStringField } }, getResult);
	}
	SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::core::undo::endGroup, { {WwiseWaapiHelper::DISPLAY_NAME, WwiseWaapiHelper::DELETE_ITEMS} }, getResult);
	OnPopulateClicked();
}

void SWaapiPicker::HandleExploreWwiseItemCommandExecute() const
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Unable to connect to localhost"));
		return;
	}

	auto& SelectedItems = GetSelectedItems();
	if (SelectedItems.Num() == 0)
		return;

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	{
		TSharedPtr<FJsonObject> from = MakeShared<FJsonObject>();
		from->SetArrayField(WwiseWaapiHelper::PATH, TArray<TSharedPtr<FJsonValue>> { MakeShared<FJsonValueString>(SelectedItems[0]->FolderPath) });
		args->SetObjectField(WwiseWaapiHelper::FROM, from);
	}

	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	options->SetArrayField(WwiseWaapiHelper::RETURN, TArray<TSharedPtr<FJsonValue>> { MakeShared<FJsonValueString>(WwiseWaapiHelper::FILEPATH) });

	TSharedPtr<FJsonObject> outJsonResult;
	if (!waapiClient->Call(ak::wwise::core::object::get, args, options, outJsonResult))
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Call Failed"));
		return;
	}

	auto Path = outJsonResult->GetArrayField(WwiseWaapiHelper::RETURN)[0]->AsObject()->GetStringField(WwiseWaapiHelper::FILEPATH);
	FPlatformProcess::ExploreFolder(*Path);
}

bool SWaapiPicker::HandleWwiseCommandCanExecute() const
{
	return GetSelectedItems().Num() == 1;
}

void SWaapiPicker::HandleFindWwiseItemInProjectExplorerCommandExecute() const
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Unable to connect to localhost"));
		return;
	}

	auto& SelectedItems = GetSelectedItems();
	if (SelectedItems.Num() == 0)
		return;

	TSharedRef<FJsonObject> args = MakeShared<FJsonObject>();
	args->SetStringField(WwiseWaapiHelper::COMMAND, WwiseWaapiHelper::FIND_IN_PROJECT_EXPLORER);
	args->SetArrayField(WwiseWaapiHelper::OBJECTS, TArray<TSharedPtr<FJsonValue>>
	{
		MakeShared<FJsonValueString>(SelectedItems[0]->ItemId.ToString(EGuidFormats::DigitsWithHyphensInBraces))
	});

	TSharedPtr<FJsonObject> getResult;
	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	if (!waapiClient->Call(ak::wwise::ui::commands::execute, args, options, getResult))
	{
		UE_LOG(LogAkAudioPicker, Log, TEXT("Call Failed"));
	}
}

void SWaapiPicker::HandleRefreshWaapiPickerCommandExecute()
{
	OnPopulateClicked();
}

void SWaapiPicker::HandleUndoWaapiPickerCommandExecute() const
{
	TSharedPtr<FJsonObject> getResult;
	SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::ui::commands::execute, { {WwiseWaapiHelper::COMMAND, WwiseWaapiHelper::UNDO} }, getResult);
}

void SWaapiPicker::HandleRedoWaapiPickerCommandExecute() const
{
	TSharedPtr<FJsonObject> getResult;
	SWaapiPickerRow::CallWaapiExecuteUri(ak::wwise::ui::commands::execute, { {WwiseWaapiHelper::COMMAND, WwiseWaapiHelper::REDO} }, getResult);
}

FReply SWaapiPicker::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyboardEvent)
{
	const FKey KeyPressed = InKeyboardEvent.GetKey();

	if ((KeyPressed == EKeys::SpaceBar))
	{
		// Play the wwise item.
		if (HandlePlayWwiseItemCommandCanExecute())
		{
			HandlePlayWwiseItemCommandExecute();
			return FReply::Handled();
		}
	}
	else if (KeyPressed == EKeys::F2)
	{
		// Rename key : Rename selected Wwise item.
		if (HandleRenameWwiseItemCommandCanExecute())
		{
			HandleRenameWwiseItemCommandExecute();
			return FReply::Handled();
		}
	}
	else if (KeyPressed == EKeys::Delete)
	{
		// Delete key : Delete selected Wwise item(s).
		if (HandleDeleteWwiseItemCommandCanExecute())
		{
			HandleDeleteWwiseItemCommandExecute();
			return FReply::Handled();
		}
	}
	else if (KeyPressed == EKeys::F5)
	{	// Populates the Waapi Picker.
		HandleRefreshWaapiPickerCommandExecute();
		return FReply::Handled();
	}
	else if ((KeyPressed == EKeys::Z) && InKeyboardEvent.IsControlDown())
	{
		// Undo
		HandleUndoWaapiPickerCommandExecute();
		return FReply::Handled();
	}
	else if ((KeyPressed == EKeys::Y) && InKeyboardEvent.IsControlDown())
	{
		// Redo
		HandleRedoWaapiPickerCommandExecute();
		return FReply::Handled();
	}
	else if (!bRestrictContextMenu && (KeyPressed == EKeys::One) && InKeyboardEvent.IsControlDown() && InKeyboardEvent.IsShiftDown())
	{
		// Finds the specified object in the Project Explorer (Sync Group 1).
		if (HandleWwiseCommandCanExecute())
		{
			HandleFindWwiseItemInProjectExplorerCommandExecute();
			return FReply::Handled();
		}
	}
	return FReply::Unhandled();
}

const TArray<TSharedPtr<FWwiseTreeItem>> SWaapiPicker::GetSelectedItems() const
{
	return TreeViewPtr->GetSelectedItems();
}

const FString SWaapiPicker::GetSearchText() const
{
	return SearchBoxFilter->GetRawFilterText().ToString();
}

const void SWaapiPicker::SetSearchText(const FString& newText)
{
	SearchBoxPtr->SetText(FText::FromString(newText));
}

EActiveTimerReturnType SWaapiPicker::SetFocusPostConstruct(double InCurrentTime, float InDeltaTime)
{
	FWidgetPath WidgetToFocusPath;
	FSlateApplication::Get().GeneratePathToWidgetUnchecked(SearchBoxPtr.ToSharedRef(), WidgetToFocusPath);
	FSlateApplication::Get().SetKeyboardFocus(WidgetToFocusPath, EFocusCause::SetDirectly);

	return EActiveTimerReturnType::Stop;
}

void SWaapiPicker::SubscribeWaapiCallbacks()
{
	struct SubscriptionData
	{
		const char* Uri;
		WampEventCallback Callback;
		uint64* SubscriptionId;
	};

	static const SubscriptionData Subscriptions[] = {
		{ak::wwise::core::object::nameChanged, WampEventCallback::CreateSP(this, &SWaapiPicker::OnWaapiRenamed), &WaapiSubscriptionIds.Renamed},
		{ak::wwise::core::object::childAdded, WampEventCallback::CreateSP(this, &SWaapiPicker::OnWaapiChildAdded), &WaapiSubscriptionIds.ChildAdded},
		{ak::wwise::core::object::childRemoved, WampEventCallback::CreateSP(this, &SWaapiPicker::OnWaapiChildRemoved), &WaapiSubscriptionIds.ChildRemoved},
	};

	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
	{
		return;
	}

	TSharedRef<FJsonObject> options = MakeShared<FJsonObject>();
	options->SetArrayField(WwiseWaapiHelper::RETURN, TArray<TSharedPtr<FJsonValue>>
	{
		MakeShared<FJsonValueString>(WwiseWaapiHelper::ID),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::NAME),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::TYPE),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::CHILDREN_COUNT),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::PATH),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::PARENT),
		MakeShared<FJsonValueString>(WwiseWaapiHelper::WORKUNIT_TYPE),
	});

	TSharedPtr<FJsonObject> result;

	for (auto& subscriptionData : Subscriptions)
	{
		if (*subscriptionData.SubscriptionId == 0)
		{
			waapiClient->Subscribe(subscriptionData.Uri,
				options,
				subscriptionData.Callback,
				*subscriptionData.SubscriptionId,
				result
			);
		}
	}
}

void SWaapiPicker::UnsubscribeWaapiCallbacks()
{
	auto waapiClient = FAkWaapiClient::Get();
	if (!waapiClient)
	{
		return;
	}

	auto doUnsubscribe = [waapiClient](uint64& subscriptionId) {
		if (subscriptionId > 0)
		{
			TSharedPtr<FJsonObject> result;
			waapiClient->Unsubscribe(subscriptionId, result);
			subscriptionId = 0;
		}
	};

	doUnsubscribe(WaapiSubscriptionIds.Renamed);
	doUnsubscribe(WaapiSubscriptionIds.ChildAdded);
	doUnsubscribe(WaapiSubscriptionIds.ChildRemoved);
}

TSharedPtr<FWwiseTreeItem> SWaapiPicker::FindTreeItemFromJsonObject(const TSharedPtr<FJsonObject>& ObjectJson, const FString& OverrideLastPart)
{
	FString objectPath;
	if (!ObjectJson->TryGetStringField(WwiseWaapiHelper::PATH, objectPath))
	{
		return {};
	}

	FString stringId;
	if (!ObjectJson->TryGetStringField(WwiseWaapiHelper::ID, stringId))
	{
		return {};
	}

	FGuid id;
	FGuid::ParseExact(stringId, EGuidFormats::DigitsWithHyphensInBraces, id);

	TArray<FString> pathParts;
	objectPath.ParseIntoArray(pathParts, *WwiseWaapiHelper::BACK_SLASH);

	if (pathParts.Num() == 0)
	{
		return {};
	}

	if (!OverrideLastPart.IsEmpty())
	{
		pathParts[pathParts.Num() - 1] = OverrideLastPart;
	}

	TSharedPtr<FWwiseTreeItem> treeItem;
	TArray<TSharedPtr<FWwiseTreeItem>>* children = &RootItems;

	FString folderPath;

	for (auto& part : pathParts)
	{
		folderPath += FString::Printf(TEXT("%s%s"), *WwiseWaapiHelper::BACK_SLASH, *part);

		bool found = false;
		for (auto& item : *children)
		{
			if (item->ItemId == id)
			{
				return item;
			}

			if (item->FolderPath == folderPath)
			{
				treeItem = item;
				children = &treeItem->Children;
				found = true;
			}
		}

		if (!found)
		{
			return {};
		}
	}

	if (treeItem.IsValid() && treeItem->ItemId != id)
	{
		return {};
	}

	return treeItem;
}

void SWaapiPicker::OnWaapiRenamed(uint64_t Id, TSharedPtr<FJsonObject> Response)
{
	FString oldName;
	if (Response->TryGetStringField(WwiseWaapiHelper::OLD_NAME, oldName) && oldName.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* objectJsonPtr = nullptr;
		if (!Response->TryGetObjectField(WwiseWaapiHelper::OBJECT, objectJsonPtr))
		{
			return;
		}

		auto& objectJson = *objectJsonPtr;

		FString stringId;
		if (!objectJson->TryGetStringField(WwiseWaapiHelper::ID, stringId))
		{
			return;
		}

		FGuid id;
		FGuid::ParseExact(stringId, EGuidFormats::DigitsWithHyphensInBraces, id);

		if (auto pendingIt = pendingTreeItems.Find(id))
		{
			CreateTreeItemWaapi(*pendingIt, objectJson);

			pendingTreeItems.Remove(id);

			AsyncTask(ENamedThreads::GameThread, [this] {
				TreeViewPtr->RequestTreeRefresh();
			});
		}

		return;
	}

	const TSharedPtr<FJsonObject>* objectJsonPtr = nullptr;
	if (Response->TryGetObjectField(WwiseWaapiHelper::OBJECT, objectJsonPtr))
	{
		TSharedPtr<FJsonObject> objectJson = *objectJsonPtr;

		auto treeItem = FindTreeItemFromJsonObject(*objectJsonPtr, oldName);

		if (treeItem)
		{
			Response->TryGetStringField(WwiseWaapiHelper::NEW_NAME, treeItem->DisplayName);
			objectJson->TryGetStringField(WwiseWaapiHelper::PATH, treeItem->FolderPath);

			if (treeItem->Parent.IsValid())
			{
				treeItem->Parent.Pin()->SortChildren();
			}

			AsyncTask(ENamedThreads::GameThread, [this] {

				TreeViewPtr->RequestTreeRefresh();
			});
		}
	}
}

template<typename ActionFunctor>
void SWaapiPicker::HandleOnWaapiChildResponse(TSharedPtr<FJsonObject> Response, const ActionFunctor& Action)
{
	const TSharedPtr<FJsonObject>* parentJsonPtr = nullptr;
	if (!Response->TryGetObjectField(WwiseWaapiHelper::PARENT, parentJsonPtr))
	{
		return;
	}

	const TSharedPtr<FJsonObject>* childJsonPtr = nullptr;
	if (!Response->TryGetObjectField(WwiseWaapiHelper::CHILD, childJsonPtr))
	{
		return;
	}

	FString childName;
	if (childJsonPtr->Get()->TryGetStringField(WwiseWaapiHelper::NAME, childName) && childName.IsEmpty())
	{
		auto parentTreeItem = FindTreeItemFromJsonObject(*parentJsonPtr);

		FString childStringId;
		childJsonPtr->Get()->TryGetStringField(WwiseWaapiHelper::ID, childStringId);
		FGuid childId;
		FGuid::ParseExact(childStringId, EGuidFormats::DigitsWithHyphensInBraces, childId);

		if (parentTreeItem && childId.IsValid())
		{
			pendingTreeItems.Add(childId, parentTreeItem);
			return;
		}
	}

	auto parentTreeItem = FindTreeItemFromJsonObject(*parentJsonPtr);

	if (parentTreeItem)
	{
		Action(parentTreeItem, *childJsonPtr);

		AsyncTask(ENamedThreads::GameThread, [this] {
			TreeViewPtr->RequestTreeRefresh();
		});
	}
}

void SWaapiPicker::OnWaapiChildAdded(uint64_t Id, TSharedPtr<FJsonObject> Response)
{
	HandleOnWaapiChildResponse(Response, [this](const TSharedPtr<FWwiseTreeItem>& parentTreeItem, const TSharedPtr<FJsonObject>& childJson) {
		CreateTreeItemWaapi(parentTreeItem, childJson);
	});
}

void SWaapiPicker::CreateTreeItemWaapi(const TSharedPtr<FWwiseTreeItem>& parentTreeItem, const TSharedPtr<FJsonObject>& childJson)
{
	if (!parentTreeItem)
	{
		return;
	}

	auto newChild = ConstructWwiseTreeItem(childJson);
	if (newChild && parentTreeItem)
	{
		newChild->Parent = parentTreeItem;

		parentTreeItem->Children.Add(newChild);
		parentTreeItem->SortChildren();

		++parentTreeItem->ChildCountInWwise;

		if (parentTreeItem->TreeRow.IsValid() && parentTreeItem->TreeRow.Pin()->IsItemExpanded())
		{
			LastExpandedItems.Add(parentTreeItem->ItemId);
		}
	}
}

void SWaapiPicker::OnWaapiChildRemoved(uint64_t Id, TSharedPtr<FJsonObject> Response)
{
	HandleOnWaapiChildResponse(Response, [this](const TSharedPtr<FWwiseTreeItem>& parentTreeItem, const TSharedPtr<FJsonObject>& childJson) {
		auto stringId = childJson->GetStringField(WwiseWaapiHelper::ID);
		FGuid id;
		FGuid::ParseExact(stringId, EGuidFormats::DigitsWithHyphensInBraces, id);

		for (int32 i = 0; i < parentTreeItem->Children.Num(); ++i)
		{
			auto& child = parentTreeItem->Children[i];
			if (child->ItemId == id)
			{
				--parentTreeItem->ChildCountInWwise;
				parentTreeItem->Children.RemoveAt(i);
				
				if (parentTreeItem->ChildCountInWwise <= 0)
				{
					LastExpandedItems.Remove(parentTreeItem->ItemId);
				}
				break;
			}
		}
	});
}

#undef LOCTEXT_NAMESPACE
