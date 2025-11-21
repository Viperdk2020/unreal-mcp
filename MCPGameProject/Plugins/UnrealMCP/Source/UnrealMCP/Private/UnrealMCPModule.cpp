#include "UnrealMCPModule.h"
#include "UnrealMCPBridge.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPModule"

// Define log category
DEFINE_LOG_CATEGORY(LogUnrealMCP);

void FUnrealMCPModule::StartupModule()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("Unreal MCP Module has started"));

	// Register console commands to start/stop the MCP servers from the editor console
	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MCP.StartServer"),
		TEXT("Start the UnrealMCP servers (legacy JSON + MCP listener)"),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GEditor)
			{
				if (UUnrealMCPBridge* Bridge = GEditor->GetEditorSubsystem<UUnrealMCPBridge>())
				{
					Bridge->StartServer();
					if (Bridge->IsRunning() || Bridge->IsMCPListenerRunning())
					{
						UE_LOG(LogUnrealMCP, Log, TEXT("MCP servers started via console command"));
					}
					else
					{
						UE_LOG(LogUnrealMCP, Warning, TEXT("MCP servers failed to start; check logs for bind errors or port conflicts"));
					}
				}
				else
				{
					UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge subsystem not available"));
				}
			}
		}),
		ECVF_Default);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MCP.StopServer"),
		TEXT("Stop the UnrealMCP servers"),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GEditor)
			{
				if (UUnrealMCPBridge* Bridge = GEditor->GetEditorSubsystem<UUnrealMCPBridge>())
				{
					Bridge->StopServer();
					UE_LOG(LogUnrealMCP, Log, TEXT("MCP servers stopped via console command"));
				}
				else
				{
					UE_LOG(LogUnrealMCP, Warning, TEXT("UnrealMCPBridge subsystem not available"));
				}
			}
		}),
		ECVF_Default);
}

void FUnrealMCPModule::ShutdownModule()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("Unreal MCP Module has shut down"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP) 
