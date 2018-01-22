// Copyright (c) 2016-2017 Codice Software - Sebastien Rombauts (sebastien.rombauts@gmail.com)

#include "PlasticSourceControlPrivatePCH.h"

#include "PlasticSourceControlMenu.h"

#include "PlasticSourceControlModule.h"
#include "PlasticSourceControlProvider.h"
#include "PlasticSourceControlOperations.h"

#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

#include "Editor.h"
#include "LevelEditor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/MessageDialog.h"
#include "EditorStyleSet.h"

#include "PackageTools.h"
#include "Engine/World.h"
#include "FileHelpers.h"

#include "Logging/MessageLog.h"

static const FName PlasticSourceControlMenuTabName("PlasticSourceControlMenu");

#define LOCTEXT_NAMESPACE "PlasticSourceControl"

void FPlasticSourceControlMenu::Register()
{
	// Register the extension with the level editor
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
		FLevelEditorModule::FLevelEditorMenuExtender ViewMenuExtender = FLevelEditorModule::FLevelEditorMenuExtender::CreateRaw(this, &FPlasticSourceControlMenu::OnExtendLevelEditorViewMenu);
		auto& MenuExtenders = LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders();
		MenuExtenders.Add(ViewMenuExtender);
		ViewMenuExtenderHandle = MenuExtenders.Last().GetHandle();
	}
}

void FPlasticSourceControlMenu::Unregister()
{
	// Unregister the level editor extensions
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
		LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll([=](const FLevelEditorModule::FLevelEditorMenuExtender& Extender) { return Extender.GetHandle() == ViewMenuExtenderHandle; });
	}
}

bool FPlasticSourceControlMenu::IsSourceControlConnected() const
{
	const ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	return Provider.IsEnabled() && Provider.IsAvailable();
}

void FPlasticSourceControlMenu::UnloadSyncReloadPackages()
{
	// Prompt to save or discard all packages
	bool bOkToExit = false;
	bool bHadPackagesToSave = false;
	{
		const bool bPromptUserToSave = true;
		const bool bSaveMapPackages = true;
		const bool bSaveContentPackages = true;
		const bool bFastSave = false;
		const bool bNotifyNoPackagesSaved = false;
		const bool bCanBeDeclined = true; // If the user clicks "don't save" this will continue and lose their changes
		bOkToExit = FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages, bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined, &bHadPackagesToSave);
	}

	// bOkToExit can be true if the user selects to not save an asset by unchecking it and clicking "save"
	if (bOkToExit)
	{
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
		FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
		bOkToExit = DirtyPackages.Num() == 0;
	}

	if (bOkToExit)
	{
		FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
		FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();

		// Unload all packages in repository
		TArray<FString> PackageRelativePaths;
		FPackageName::FindPackagesInDirectory(PackageRelativePaths, *Provider.GetPathToWorkspaceRoot());

		TArray<FString> PackageNames;
		PackageNames.Reserve(PackageRelativePaths.Num());
		for(const FString& Path : PackageRelativePaths)
		{
			FString PackageName;
			FString FailureReason;
			if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackageName, &FailureReason))
			{
				PackageNames.Add(PackageName);
			}
			else
			{
				FMessageLog("PlasticSourceControl").Error(FText::FromString(FailureReason));
			}
		}

		TArray<FString> PackageNamesToReload;
		FString WorldPackageFilenameToReload;
		PreparePackagesForReload(PackageNames, PackageNamesToReload, WorldPackageFilenameToReload);

		// Prepare a "Sync" Source Control operations synchronously
		TSharedRef<FSync, ESPMode::ThreadSafe> SyncOperation = ISourceControlOperation::Create<FSync>();
		TArray<FString> WorkspaceRoot;
		WorkspaceRoot.Add(Provider.GetPathToWorkspaceRoot()); // Revert the root of the workspace (needs an absolute path)

		// Display an ongoing notification during the whole operation
		DisplayInProgressNotification(SyncOperation->GetInProgressString());
		const ECommandResult::Type Result = Provider.Execute(SyncOperation, WorkspaceRoot);
		OnSourceControlOperationComplete(SyncOperation, Result);
		ReloadPackages(PackageNamesToReload, WorldPackageFilenameToReload);
	}
	else
	{
		FMessageLog ErrorMessage("PlasticSourceControl");
		ErrorMessage.Warning(LOCTEXT("SourceControlMenu_Sync_Unsaved", "Save All Assets before attempting to Sync!"));
		ErrorMessage.Notify();
	}

}

void FPlasticSourceControlMenu::PreparePackagesForReload(const TArray<FString>& InPackageNames, TArray<FString>& OutPackageNamesToReload, FString& OutWorldPackageFilenameToReload)
{
	// Inspired from ContentBrowserUtils.cpp and PackageRestore.cpp
	if (InPackageNames.Num() > 0)
	{
		// Form a list of loaded packages to unload
		TArray<UPackage*> LoadedPackages;
		LoadedPackages.Reserve(InPackageNames.Num());
		for(const FString& PackageName : InPackageNames)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (Package != nullptr)
			{
				LoadedPackages.Add(Package);
				OutPackageNamesToReload.Add(Package->GetName());
			}
		}

		const int32 NumLoadedWorldFound = LoadedPackages.RemoveAll([&](UPackage* Package)
			{
				const UWorld* ExistingWorld = UWorld::FindWorldInPackage(Package);
				if (ExistingWorld && ExistingWorld->WorldType == EWorldType::Editor)
				{
					return true;
				}
				return false;
			});

		if (NumLoadedWorldFound > 0)
		{
			UPackage* const CurrentWorldPackage = CastChecked<UPackage>(GWorld->GetOuter());
			OutWorldPackageFilenameToReload = USourceControlHelpers::PackageFilename(CurrentWorldPackage);

			// Remove the Map from the package list to reload it separately, at the very last step
			OutPackageNamesToReload.Remove(CurrentWorldPackage->GetName());

			// Replaces currently loaded world by an empty new world. Note that this may fail.
			// This will make the UPackages from loaded worlds invalid, thankfully they have already been removed from LoadedPackages
			// This will also prompt for save and can be skipped by the user
			GEditor->CreateNewMapForEditing();
		}

		FText ErrorMessage;
		PackageTools::UnloadPackages(LoadedPackages, ErrorMessage);
		if (!ErrorMessage.IsEmpty())
		{
			FMessageDialog::Open(EAppMsgType::Ok, ErrorMessage);
		}
	}
}

void FPlasticSourceControlMenu::ReloadPackages(const TArray<FString>& InPackageNamesToReload, const FString& InWorldPackageFilenameToReload)
{
	UE_LOG(LogSourceControl, Log, TEXT("Reloading %d Packages..."), InPackageNamesToReload.Num());

	// Inspired from ContentBrowserUtils.cpp and PackageRestore.cpp
	for(const FString& PackageName : InPackageNamesToReload)
	{
		PackageTools::LoadPackage(PackageName);
	}

	// Also reload the current world if we caused it to be unloaded
	if (!InWorldPackageFilenameToReload.IsEmpty())
	{
		FEditorFileUtils::LoadMap(InWorldPackageFilenameToReload);
	}
}

void FPlasticSourceControlMenu::SyncProjectClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		UnloadSyncReloadPackages();
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

void FPlasticSourceControlMenu::RevertUnchangedClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch a "RevertUnchanged" Operation
		FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
		FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
		TSharedRef<FPlasticRevertUnchanged, ESPMode::ThreadSafe> RevertUnchangedOperation = ISourceControlOperation::Create<FPlasticRevertUnchanged>();
		TArray<FString> WorkspaceRoot;
		WorkspaceRoot.Add(Provider.GetPathToWorkspaceRoot()); // Revert the root of the workspace (needs an absolute path)
		ECommandResult::Type Result = Provider.Execute(RevertUnchangedOperation, WorkspaceRoot, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FPlasticSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RevertUnchangedOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RevertUnchangedOperation->GetName());
		}
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

void FPlasticSourceControlMenu::RevertAllClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Ask the user before reverting all!
		const FText DialogText(LOCTEXT("SourceControlMenu_AskRevertAll", "Revert all modifications into the workspace?"));
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
		if (Choice == EAppReturnType::Ok)
		{
			// Launch a "RevertAll" Operation
			FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
			FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
			TSharedRef<FPlasticRevertAll, ESPMode::ThreadSafe> RevertAllOperation = ISourceControlOperation::Create<FPlasticRevertAll>();
			TArray<FString> WorkspaceRoot;
			WorkspaceRoot.Add(Provider.GetPathToWorkspaceRoot()); // Revert the root of the workspace (needs an absolute path)
			ECommandResult::Type Result = Provider.Execute(RevertAllOperation, WorkspaceRoot, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FPlasticSourceControlMenu::OnSourceControlOperationComplete));
			if (Result == ECommandResult::Succeeded)
			{
				// Display an ongoing notification during the whole operation
				DisplayInProgressNotification(RevertAllOperation->GetInProgressString());
			}
			else
			{
				// Report failure with a notification
				DisplayFailureNotification(RevertAllOperation->GetName());
			}
		}
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

void FPlasticSourceControlMenu::RefreshClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch an "UpdateStatus" Operation
		FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>("PlasticSourceControl");
		FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FUpdateStatus>();
		RefreshOperation->SetCheckingAllFiles(true);
		RefreshOperation->SetGetOpenedOnly(true);
		ECommandResult::Type Result = Provider.Execute(RefreshOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FPlasticSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RefreshOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RefreshOperation->GetName());
		}
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

// Display an ongoing notification during the whole operation
void FPlasticSourceControlMenu::DisplayInProgressNotification(const FText& InOperationInProgressString)
{
	if (!OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InOperationInProgressString);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (OperationInProgressNotification.IsValid())
		{
			OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

// Remove the ongoing notification at the end of the operation
void FPlasticSourceControlMenu::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void FPlasticSourceControlMenu::DisplaySucessNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Success", "{0} operation was successful!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
	Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
	FMessageLog("LogSourceControl").Info(NotificationText);
}

// Display a temporary failure notification at the end of the operation
void FPlasticSourceControlMenu::DisplayFailureNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Failure", "Error: {0} operation failed!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	FMessageLog("LogSourceControl").Info(NotificationText);
}

void FPlasticSourceControlMenu::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySucessNotification(InOperation->GetName());
	}
	else
	{
		DisplayFailureNotification(InOperation->GetName());
	}
}

void FPlasticSourceControlMenu::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(
		LOCTEXT("PlasticSync",			"Sync/Update Workspace"),
		LOCTEXT("PlasticSyncTooltip",	"Update all files in the workspace to the latest version."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Sync"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FPlasticSourceControlMenu::SyncProjectClicked),
			FCanExecuteAction::CreateRaw(this, &FPlasticSourceControlMenu::IsSourceControlConnected)
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("PlasticRevertUnchanged",			"Revert Unchanged"),
		LOCTEXT("PlasticRevertUnchangedTooltip",	"Revert checked-out but unchanged files in the workspace."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FPlasticSourceControlMenu::RevertUnchangedClicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("PlasticRevertAll",			"Revert All"),
		LOCTEXT("PlasticRevertAllTooltip",	"Revert all files in the workspace to their controlled/unchanged state."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Revert"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FPlasticSourceControlMenu::RevertAllClicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("PlasticRefresh",			"Refresh All"),
		LOCTEXT("PlasticRefreshTooltip",	"Update the source control status of all files in the workspace."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Refresh"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FPlasticSourceControlMenu::RefreshClicked),
			FCanExecuteAction()
		)
	);
}

TSharedRef<FExtender> FPlasticSourceControlMenu::OnExtendLevelEditorViewMenu(const TSharedRef<FUICommandList> CommandList)
{
	TSharedRef<FExtender> Extender(new FExtender());

	Extender->AddMenuExtension(
		"SourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FPlasticSourceControlMenu::AddMenuExtension));

	return Extender;
}

#undef LOCTEXT_NAMESPACE
