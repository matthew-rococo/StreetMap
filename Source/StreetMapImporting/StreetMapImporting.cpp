// Copyright 2017 Mike Fricker. All Rights Reserved.

#include "StreetMapImporting.h"
#include "StreetMapAssetTypeActions.h"
#include "ModuleManager.h"
#include "StreetMapStyle.h"
#include "StreetMapComponentDetails.h"
#include "StreetMapCommands.h"
#include "Misc/MessageDialog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "LevelEditor.h"
#include "StreetMap.h"
#include "XmlFile.h"



IMPLEMENT_MODULE( FStreetMapImportingModule, StreetMapImporting )

#define LOCTEXT_NAMESPACE "FStreetMapImporting"


void FStreetMapImportingModule::ShowErrorMessage(const FText& MessageText)
{
	FNotificationInfo Info(MessageText);
	Info.ExpireDuration = 5.0f;
	Info.bUseLargeFont = false;
	Info.bUseSuccessFailIcons = true;
	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Notification->ExpireAndFadeout();
	}
}

void FStreetMapImportingModule::ShowInfoMessage(const FText& MessageText)
{
	FNotificationInfo Info(MessageText);
	Info.ExpireDuration = 5.0f;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = true;
	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Success);
		Notification->ExpireAndFadeout();
	}
}

void FStreetMapImportingModule::StartupModule()
{
	// Register asset types
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>( "AssetTools" ).Get();
	StreetMapAssetTypeActions = MakeShareable( new FStreetMapAssetTypeActions() );
	AssetTools.RegisterAssetTypeActions( StreetMapAssetTypeActions.ToSharedRef() );

	// Initialize & Register StreetMap Style
	FStreetMapStyle::Initialize();
	FStreetMapStyle::ReloadTextures();

	// Register StreetMapComponent Detail Customization
	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout("StreetMapComponent", FOnGetDetailCustomizationInstance::CreateStatic(&FStreetMapComponentDetails::MakeInstance));
	PropertyModule.NotifyCustomizationModuleChanged();

	// Registering Visualizer
	if (GUnrealEd != NULL)
	{
		TSharedPtr<FComponentVisualizer> Visualizer = MakeShareable(new FStreetMapComponentVisualizer());

		if (Visualizer.IsValid())
		{
			GUnrealEd->RegisterComponentVisualizer(UStreetMapComponent::StaticClass()->GetFName(), Visualizer);
			Visualizer->OnRegister();
		}

	}

	// Toolbar plugin code
	FStreetMapCommands::Register();

	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FStreetMapCommands::Get().SaveOsmFile,
		FExecuteAction::CreateRaw(this, &FStreetMapImportingModule::SaveOsmButtonClicked),
		FCanExecuteAction());

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");

	{
		TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
		MenuExtender->AddMenuExtension("FileLoadAndSave", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FStreetMapImportingModule::AddMenuExtension));

		LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
	}

	{
		TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
		ToolbarExtender->AddToolBarExtension("File", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FStreetMapImportingModule::AddToolbarExtension));

		LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
}

void FStreetMapImportingModule::ShutdownModule()
{
	// Unregister all the asset types that we registered
	if( FModuleManager::Get().IsModuleLoaded( "AssetTools" ) )
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>( "AssetTools" ).Get();
		AssetTools.UnregisterAssetTypeActions( StreetMapAssetTypeActions.ToSharedRef() );
		StreetMapAssetTypeActions.Reset();
	}

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout("StreetMapComponent");
		PropertyModule.NotifyCustomizationModuleChanged();
	}

	// Unregister Visualizer
	if (GUnrealEd != NULL)
	{
		GUnrealEd->UnregisterComponentVisualizer(UStreetMapComponent::StaticClass()->GetFName());
	}

	// Unregister StreetMap Style
	FStreetMapStyle::Shutdown();

	FStreetMapCommands::Unregister();
}

void FStreetMapImportingModule::SaveOsmButtonClicked()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorSaveNoWorld", "No editor world, please create a new level and add a StreetMap to it")
		);
		ShowErrorMessage(DialogText);
		return;
	}

	TArray<AActor*> AllStreetMapActors;
	UGameplayStatics::GetAllActorsOfClass(World, AStreetMapActor::StaticClass(), AllStreetMapActors);

	if (!AllStreetMapActors.Num())
	{
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorSaveNoActor", "No StreetMap Actor found in the level, please add one by dragging an OSM asset into the editor")
		);
		ShowErrorMessage(DialogText);
		return;
	}
	else if (AllStreetMapActors.Num() != 1)
	{
		// More than one StreetMapActor is currently unsupported. Still aks user to save the first one
		FText DialogText = FText::Format(
			LOCTEXT("StreetMapEditorSaveMoreThanOneActor", "More than one StreetMap Actor found in the level, should Actor {0} be saved?"),
			FText::FromString(AllStreetMapActors[0]->GetActorLabel())
		);
		auto DialogAnswer = FMessageDialog::Open(EAppMsgType::YesNo, DialogText);
		if (DialogAnswer == EAppReturnType::No || DialogAnswer == EAppReturnType::Cancel)
		{
			return;
		}
	}

	// One OSM file, so save it
	AStreetMapActor* MapActor = Cast<AStreetMapActor>(AllStreetMapActors[0]);
	if (!MapActor)
	{
		// should never happen
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorCriticalError", "Failed save. Casting to AStreetMapActor failed!")
		);
		ShowErrorMessage(DialogText);
		return;
	}

	UStreetMap* StreetMap = MapActor->GetStreetMapComponent()->GetStreetMap();
	if (!StreetMap)
	{
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorSaveNoStreetMapInActor", "Found an StreetMapActor but it has no StreetMap asset assign. Aborting save!")
		);
		ShowErrorMessage(DialogText);
		return;
	}

	// Get OSM file
	UOSMFile* OSMFile = StreetMap->OSMFile;
	if (!OSMFile)
	{
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorSaveNoOsmFileInStreetMap", "Found an StreetMap but it has no OSM file assign to it. Aborting save!")
		);
		ShowErrorMessage(DialogText);
		return;
	}

	// load file as it might be imported in an previous instance of the editor
	if (!OSMFile->OsmXmlFile.IsValid())
	{
		if (!OSMFile->LoadOpenStreetMapFile(OSMFile->OSMFileLocation, false, nullptr))
		{
			FText DialogText = FText(
				LOCTEXT("StreetMapEditorCriticalError2", "OSM file could not be parsed correctly. Aborting! Did it change after importing the asset?")
			);
			ShowErrorMessage(DialogText);
			return;
		}
	}

	// TODO: gather changed actors, like in StreetMapRoadFurniture.cpp

	// now finally save the file
	if (OSMFile->SaveOpenStreetMapFile())
	{
		FText DialogText = FText(
			LOCTEXT("StreetMapEditorCriticalError3", "Tried saving the OSM file but failed!")
		);
		ShowErrorMessage(DialogText);
		return;
	}
	
	FText DialogText = FText::Format(
		LOCTEXT("StreetMapEditorSaveSuccess", "OSM file saved to {0}"),
		FText::FromString(OSMFile->OSMFileLocation)
	);
	ShowInfoMessage(DialogText);
	return;
}

void FStreetMapImportingModule::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(FStreetMapCommands::Get().SaveOsmFile);
}

void FStreetMapImportingModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(FStreetMapCommands::Get().SaveOsmFile);
}

#undef LOCTEXT_NAMESPACE