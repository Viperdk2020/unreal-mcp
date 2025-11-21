#include "MCPProtocolServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "UnrealMCPModule.h"
#include "MCPJsonHelpers.h"
#include "MCPSettings.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"

namespace { constexpr int32 MCPProtocolChunkSize = 65536; }

FMCPProtocolServerRunnable::FMCPProtocolServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
	: Bridge(InBridge)
	, ListenerSocket(InListenerSocket)
	, bRunning(true)
	, StartTimeSeconds(FPlatformTime::Seconds())
{
	UE_LOG(LogUnrealMCP, Log, TEXT("MCP protocol server runnable created"));
}

FMCPProtocolServerRunnable::~FMCPProtocolServerRunnable()
{
}

bool FMCPProtocolServerRunnable::Init()
{
	return true;
}

uint32 FMCPProtocolServerRunnable::Run()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("MCP protocol server thread started"));

	while (bRunning)
	{
		bool bPending = false;
		if (ListenerSocket->HasPendingConnection(bPending) && bPending)
		{
			ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPProtocolClient")));
			if (ClientSocket.IsValid())
			{
				UE_LOG(LogUnrealMCP, Log, TEXT("MCP protocol client connected"));

				// Configure socket
				ClientSocket->SetNoDelay(true);
				int32 SocketBufferSize = 65536;
				ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
				ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
				ClientSocket->SetNonBlocking(false);

				HandleClientConnection(ClientSocket);
				UE_LOG(LogUnrealMCP, Log, TEXT("MCP protocol client disconnected"));
			}
			else
			{
				UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to accept MCP protocol client connection"));
			}
		}

		FPlatformProcess::Sleep(0.1f);
	}

	UE_LOG(LogUnrealMCP, Log, TEXT("MCP protocol server thread stopped"));
	return 0;
}

void FMCPProtocolServerRunnable::Stop()
{
	bRunning = false;
}

void FMCPProtocolServerRunnable::Exit()
{
}

void FMCPProtocolServerRunnable::HandleClientConnection(TSharedPtr<FSocket> InClientSocket)
{
	if (!InClientSocket.IsValid())
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Invalid MCP protocol client socket"));
		return;
	}

	const UMCPSettings* Settings = GetDefault<UMCPSettings>();
	const int32 MaxBufferSize = Settings->MaxMessageSizeMB * 1024 * 1024;

	FMCPDynamicBuffer MessageBuffer;
	TArray<uint8> ChunkBuffer;
	ChunkBuffer.SetNum(MCPProtocolChunkSize);

	double LastHeartbeatTime = FPlatformTime::Seconds();

	while (bRunning)
	{
		int32 BytesRead = 0;
		bool bReadSuccess = InClientSocket->Recv(ChunkBuffer.GetData(), MCPProtocolChunkSize, BytesRead, ESocketReceiveFlags::None);

		if (BytesRead > 0)
		{
			MessageBuffer.Append(ChunkBuffer.GetData(), BytesRead);

			TArray<FString> Messages;
			int32 NumExtracted = MessageBuffer.ExtractMessages(Messages);

			if (NumExtracted > 0)
			{
				UE_LOG(LogUnrealMCP, VeryVerbose, TEXT("MCP protocol extracted %d message(s)"), NumExtracted);

				for (const FString& Message : Messages)
				{
					ProcessMessage(InClientSocket, Message);
				}
			}

			if (MessageBuffer.GetSize() > MaxBufferSize)
			{
				UE_LOG(LogUnrealMCP, Error, TEXT("MCP protocol message buffer exceeded %dMB, disconnecting"), Settings->MaxMessageSizeMB);
				break;
			}
		}
		else if (!bReadSuccess)
		{
			int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
			if (LastError != SE_EWOULDBLOCK)
			{
				UE_LOG(LogUnrealMCP, Warning, TEXT("MCP protocol connection error: %d"), LastError);
				break;
			}
		}

		SendHeartbeatIfNeeded(InClientSocket, LastHeartbeatTime);
		FPlatformProcess::Sleep(0.01f);
	}
}

void FMCPProtocolServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
	TSharedPtr<FJsonObject> JsonMessage;
	FString ErrorMessage;

	if (!FMCPJsonHelpers::ParseJson(Message, JsonMessage, ErrorMessage))
	{
		TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(ErrorMessage);
		FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	if (!JsonMessage->HasField(TEXT("type")))
	{
		TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(TEXT("Missing required field: type"));
		FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	const FString Type = JsonMessage->GetStringField(TEXT("type"));

	if (Type.Equals(TEXT("ping")))
	{
		TSharedPtr<FJsonObject> Pong = MakeShareable(new FJsonObject());
		Pong->SetStringField(TEXT("type"), TEXT("pong"));
		FString Response = FMCPJsonHelpers::SerializeJson(Pong) + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	if (Type.Equals(TEXT("status")))
	{
		FString Response = BuildStatusPayload() + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	if (Type.Equals(TEXT("tools")))
	{
		FString Response = BuildToolsPayload() + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	if (Type.Equals(TEXT("call_tool")))
	{
		if (!JsonMessage->HasField(TEXT("tool")))
		{
			TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(TEXT("Missing required field: tool"));
			FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
			SendAll(Client, Response);
			return;
		}

		const FString ToolName = JsonMessage->GetStringField(TEXT("tool"));
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
		if (JsonMessage->HasField(TEXT("params")))
		{
			TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
			if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
			{
				Params = ParamsValue->AsObject();
			}
		}

		const FString RawResult = Bridge->ExecuteCommand(ToolName, Params);

		// ExecuteCommand already returns serialized JSON
		FString Response = RawResult + TEXT("\n");
		SendAll(Client, Response);
		return;
	}

	TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonHelpers::CreateErrorResponse(
		FString::Printf(TEXT("Unknown MCP protocol message type: %s"), *Type)
	);
	FString Response = FMCPJsonHelpers::SerializeJson(ErrorResponse) + TEXT("\n");
	SendAll(Client, Response);
}

bool FMCPProtocolServerRunnable::SendAll(TSharedPtr<FSocket> Client, const FString& Message)
{
	if (!Client.IsValid())
	{
		return false;
	}

	FTCHARToUTF8 Converter(*Message);
	const uint8* Data = reinterpret_cast<const uint8*>(Converter.Get());
	int32 TotalBytes = Converter.Length();

	int32 BytesSent = 0;
	while (BytesSent < TotalBytes)
	{
		int32 ChunkSent = 0;
		if (!Client->Send(Data + BytesSent, TotalBytes - BytesSent, ChunkSent))
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("MCP protocol socket send failed after %d/%d bytes"), BytesSent, TotalBytes);
			return false;
		}

		if (ChunkSent <= 0)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("MCP protocol socket send returned %d bytes, aborting send"), ChunkSent);
			return false;
		}

		BytesSent += ChunkSent;
	}

	return true;
}

void FMCPProtocolServerRunnable::SendHeartbeatIfNeeded(TSharedPtr<FSocket> Client, double& LastHeartbeatTime)
{
	const UMCPSettings* Settings = GetDefault<UMCPSettings>();
	if (Settings->HeartbeatInterval <= 0.0f)
	{
		return;
	}

	double CurrentTime = FPlatformTime::Seconds();
	if (CurrentTime - LastHeartbeatTime < Settings->HeartbeatInterval)
	{
		return;
	}

	TSharedPtr<FJsonObject> HeartbeatObj = MakeShareable(new FJsonObject());
	HeartbeatObj->SetStringField(TEXT("type"), TEXT("heartbeat"));
	HeartbeatObj->SetNumberField(TEXT("timestamp"), CurrentTime);

	FString HeartbeatMsg = FMCPJsonHelpers::SerializeJson(HeartbeatObj) + TEXT("\n");
	if (SendAll(Client, HeartbeatMsg))
	{
		UE_LOG(LogUnrealMCP, VeryVerbose, TEXT("MCP protocol heartbeat sent"));
		LastHeartbeatTime = CurrentTime;
	}
	else
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to send MCP protocol heartbeat"));
	}
}

FString FMCPProtocolServerRunnable::BuildStatusPayload() const
{
	TSharedPtr<FJsonObject> StatusObj = MakeShareable(new FJsonObject());
	StatusObj->SetStringField(TEXT("type"), TEXT("status"));
	StatusObj->SetBoolField(TEXT("running"), true);
	StatusObj->SetNumberField(TEXT("uptime_seconds"), FPlatformTime::Seconds() - StartTimeSeconds);
	const UMCPSettings* Settings = GetDefault<UMCPSettings>();
	StatusObj->SetStringField(TEXT("host"), Settings->ServerHost);
	StatusObj->SetNumberField(TEXT("port"), Settings->MCPListenerPort);
	StatusObj->SetNumberField(TEXT("heartbeat_interval"), Settings->HeartbeatInterval);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(StatusObj.ToSharedRef(), Writer);
	return Output;
}

FString FMCPProtocolServerRunnable::BuildToolsPayload() const
{
	TSharedPtr<FJsonObject> ToolsObj = MakeShareable(new FJsonObject());
	ToolsObj->SetStringField(TEXT("type"), TEXT("tools"));

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// Note: this list mirrors the switch in UUnrealMCPBridge::ExecuteCommand
	auto AddTool = [&ToolsArray](const FString& Name, const FString& Description)
	{
		TSharedPtr<FJsonObject> ToolObj = MakeShareable(new FJsonObject());
		ToolObj->SetStringField(TEXT("name"), Name);
		ToolObj->SetStringField(TEXT("description"), Description);
		ToolsArray.Add(MakeShareable(new FJsonValueObject(ToolObj)));
	};

	AddTool(TEXT("ping"), TEXT("Simple connectivity test (returns pong)"));
	AddTool(TEXT("get_actors_in_level"), TEXT("List all actors in the current level"));
	AddTool(TEXT("find_actors_by_name"), TEXT("Find actors by display label pattern"));
	AddTool(TEXT("spawn_actor"), TEXT("Spawn an actor of a given type"));
	AddTool(TEXT("delete_actor"), TEXT("Delete an actor by name"));
	AddTool(TEXT("set_actor_transform"), TEXT("Set transform for an actor"));
	AddTool(TEXT("get_actor_properties"), TEXT("Get properties for an actor"));
	AddTool(TEXT("set_actor_property"), TEXT("Set a property on an actor"));
	AddTool(TEXT("spawn_blueprint_actor"), TEXT("Spawn an actor from a Blueprint"));
	AddTool(TEXT("focus_viewport"), TEXT("Focus viewport on a target"));
	AddTool(TEXT("take_screenshot"), TEXT("Trigger editor screenshot"));
	AddTool(TEXT("create_blueprint"), TEXT("Create a new Blueprint asset"));
	AddTool(TEXT("add_component_to_blueprint"), TEXT("Add a component to a Blueprint"));
	AddTool(TEXT("set_component_property"), TEXT("Set a component property on a Blueprint"));
	AddTool(TEXT("set_physics_properties"), TEXT("Configure physics properties"));
	AddTool(TEXT("compile_blueprint"), TEXT("Compile a Blueprint"));
	AddTool(TEXT("set_blueprint_property"), TEXT("Set a Blueprint property"));
	AddTool(TEXT("set_static_mesh_properties"), TEXT("Configure static mesh properties"));
	AddTool(TEXT("set_pawn_properties"), TEXT("Configure pawn properties"));
	AddTool(TEXT("connect_blueprint_nodes"), TEXT("Connect two Blueprint graph nodes"));
	AddTool(TEXT("add_blueprint_get_self_component_reference"), TEXT("Add a get reference node to a component"));
	AddTool(TEXT("add_blueprint_self_reference"), TEXT("Add a self reference node"));
	AddTool(TEXT("find_blueprint_nodes"), TEXT("Find nodes in a Blueprint graph"));
	AddTool(TEXT("add_blueprint_event_node"), TEXT("Add an event node to a Blueprint graph"));
	AddTool(TEXT("add_blueprint_input_action_node"), TEXT("Add an input action node"));
	AddTool(TEXT("add_blueprint_function_node"), TEXT("Add a function call node"));
	AddTool(TEXT("add_blueprint_get_component_node"), TEXT("Add a get component node"));
	AddTool(TEXT("add_blueprint_variable"), TEXT("Add a variable to a Blueprint"));
	AddTool(TEXT("create_input_mapping"), TEXT("Create a project input mapping"));
	AddTool(TEXT("create_umg_widget_blueprint"), TEXT("Create a UMG Widget Blueprint"));
	AddTool(TEXT("add_text_block_to_widget"), TEXT("Add a TextBlock to a widget"));
	AddTool(TEXT("add_button_to_widget"), TEXT("Add a Button to a widget"));
	AddTool(TEXT("bind_widget_event"), TEXT("Bind a widget event"));
	AddTool(TEXT("set_text_block_binding"), TEXT("Set a binding on a TextBlock widget"));
	AddTool(TEXT("add_widget_to_viewport"), TEXT("Add a widget to the viewport"));

	ToolsObj->SetArrayField(TEXT("tools"), ToolsArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(ToolsObj.ToSharedRef(), Writer);
	return Output;
}
