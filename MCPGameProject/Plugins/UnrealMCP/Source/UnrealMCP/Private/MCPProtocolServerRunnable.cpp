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
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"
#include "Misc/Guid.h"

namespace
{
constexpr int32 MCPProtocolChunkSize = 65536;

struct FMCPHttpRequest
{
	FString Method;
	FString Path;
	TMap<FString, FString> Headers;
	FString Body;
};

int32 FindHttpBodyStart(const uint8* Data, int32 Size)
{
	// Look for CRLFCRLF terminator first
	for (int32 i = 0; i <= Size - 4; ++i)
	{
		if (Data[i] == '\r' && Data[i + 1] == '\n' && Data[i + 2] == '\r' && Data[i + 3] == '\n')
		{
			return i + 4;
		}
	}

	// Fallback to bare LFLF
	for (int32 i = 0; i <= Size - 2; ++i)
	{
		if (Data[i] == '\n' && Data[i + 1] == '\n')
		{
			return i + 2;
		}
	}

	return INDEX_NONE;
}

int32 ParseContentLength(const FString& Headers)
{
	TArray<FString> Lines;
	Headers.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		int32 ColonIndex;
		if (Line.FindChar(TEXT(':'), ColonIndex))
		{
			FString Key = Line.Left(ColonIndex).TrimStartAndEnd();
			if (Key.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
			{
				FString Value = Line.Mid(ColonIndex + 1).TrimStartAndEnd();
				return FCString::Atoi(*Value);
			}
		}
	}

	return 0;
}

FString GetStatusText(int32 StatusCode)
{
	switch (StatusCode)
	{
	case 200: return TEXT("OK");
	case 202: return TEXT("Accepted");
	case 400: return TEXT("Bad Request");
	case 404: return TEXT("Not Found");
	case 405: return TEXT("Method Not Allowed");
	case 500: return TEXT("Internal Server Error");
	default:  return TEXT("OK");
	}
}

TSharedPtr<FJsonObject> BuildJsonRpcResponse(const TSharedPtr<FJsonValue>& IdValue, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (IdValue.IsValid())
	{
		Response->SetField(TEXT("id"), IdValue);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("result"), Result);
	return Response;
}

TSharedPtr<FJsonObject> BuildJsonRpcError(const TSharedPtr<FJsonValue>& IdValue, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (IdValue.IsValid())
	{
		Response->SetField(TEXT("id"), IdValue);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}

FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Output;
}
} // namespace

FMCPProtocolServerRunnable::FMCPProtocolServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
	: Bridge(InBridge)
	, ListenerSocket(InListenerSocket)
	, bRunning(true)
	, StartTimeSeconds(FPlatformTime::Seconds())
	, SessionId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
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
				ClientSocket->SetNonBlocking(true);

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
	const double ReceiveTimeout = Settings->CommandTimeout > 0.0f ? Settings->CommandTimeout : 0.0;

	FMCPDynamicBuffer Buffer;
	TArray<uint8> ChunkBuffer;
	ChunkBuffer.SetNum(MCPProtocolChunkSize);

	int32 BodyStartIndex = INDEX_NONE;
	int32 ContentLength = 0;

	const double StartTime = FPlatformTime::Seconds();

	while (bRunning)
	{
		int32 BytesRead = 0;
		const bool bReadSuccess = InClientSocket->Recv(ChunkBuffer.GetData(), MCPProtocolChunkSize, BytesRead, ESocketReceiveFlags::None);

		if (BytesRead > 0)
		{
			Buffer.Append(ChunkBuffer.GetData(), BytesRead);

			if (BodyStartIndex == INDEX_NONE)
			{
				BodyStartIndex = FindHttpBodyStart(Buffer.GetData(), Buffer.GetSize());
				if (BodyStartIndex != INDEX_NONE)
				{
					TArray<uint8> HeaderBytes;
					HeaderBytes.Append(Buffer.GetData(), BodyStartIndex);
					HeaderBytes.Add(0); // Null terminate for safe conversion
					const FString HeaderString = UTF8_TO_TCHAR(HeaderBytes.GetData());
					ContentLength = ParseContentLength(HeaderString);
				}
			}

			if (BodyStartIndex != INDEX_NONE && Buffer.GetSize() >= BodyStartIndex + ContentLength)
			{
				TArray<uint8> RequestBytes;
				RequestBytes.Append(Buffer.GetData(), BodyStartIndex + ContentLength);
				RequestBytes.Add(0);
				const FString RawRequest = UTF8_TO_TCHAR(RequestBytes.GetData());

				HandleHttpRequest(InClientSocket, RawRequest);
				break;
			}
		}
		else if (!bReadSuccess)
		{
			const int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
			if (LastError != SE_EWOULDBLOCK)
			{
				UE_LOG(LogUnrealMCP, Warning, TEXT("MCP protocol connection error: %d"), LastError);
				break;
			}
		}

		if (ReceiveTimeout > 0.0 && (FPlatformTime::Seconds() - StartTime) > ReceiveTimeout)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("MCP protocol HTTP request timed out"));
			break;
		}

		FPlatformProcess::Sleep(0.005f);
	}

	InClientSocket->Close();
}

bool FMCPProtocolServerRunnable::HandleHttpRequest(TSharedPtr<FSocket> Client, const FString& RawRequest)
{
	if (!Client.IsValid())
	{
		return false;
	}

	int32 HeaderEnd = RawRequest.Find(TEXT("\r\n\r\n"));
	int32 DelimiterLength = 4;
	if (HeaderEnd == INDEX_NONE)
	{
		HeaderEnd = RawRequest.Find(TEXT("\n\n"));
		DelimiterLength = HeaderEnd == INDEX_NONE ? 0 : 2;
	}

	if (HeaderEnd == INDEX_NONE)
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Received malformed HTTP request (missing header terminator)"));
		SendHttpResponse(Client, TEXT("Invalid HTTP request"), TEXT("text/plain"), 400);
		return false;
	}

	const FString HeaderPart = RawRequest.Left(HeaderEnd);
	const FString BodyPart = RawRequest.Mid(HeaderEnd + DelimiterLength);

	FMCPHttpRequest Request;
	TArray<FString> HeaderLines;
	HeaderPart.ParseIntoArrayLines(HeaderLines);

	if (HeaderLines.Num() == 0)
	{
		SendHttpResponse(Client, TEXT("Invalid HTTP request"), TEXT("text/plain"), 400);
		return false;
	}

	TArray<FString> RequestLineParts;
	HeaderLines[0].ParseIntoArray(RequestLineParts, TEXT(" "), true);
	if (RequestLineParts.Num() < 2)
	{
		SendHttpResponse(Client, TEXT("Invalid request line"), TEXT("text/plain"), 400);
		return false;
	}

	Request.Method = RequestLineParts[0].ToUpper();
	Request.Path = RequestLineParts[1];

	for (int32 Index = 1; Index < HeaderLines.Num(); ++Index)
	{
		int32 ColonIndex;
		if (HeaderLines[Index].FindChar(TEXT(':'), ColonIndex))
		{
			const FString Key = HeaderLines[Index].Left(ColonIndex).TrimStartAndEnd();
			const FString Value = HeaderLines[Index].Mid(ColonIndex + 1).TrimStartAndEnd();
			Request.Headers.Add(Key, Value);
		}
	}

	Request.Body = BodyPart;

	if (Request.Method == TEXT("GET"))
	{
		TMap<FString, FString> Headers;
		Headers.Add(TEXT("Cache-Control"), TEXT("no-cache, no-transform"));
		Headers.Add(TEXT("Connection"), TEXT("close"));
		return SendHttpResponse(Client, TEXT(""), TEXT("text/event-stream"), 200, Headers);
	}

	if (Request.Method != TEXT("POST"))
	{
		return SendHttpResponse(Client, TEXT("Method Not Allowed"), TEXT("text/plain"), 405);
	}

	TSharedPtr<FJsonObject> JsonMessage;
	FString ErrorMessage;
	if (!FMCPJsonHelpers::ParseJson(Request.Body, JsonMessage, ErrorMessage))
	{
		const FString Payload = SerializeJsonObject(BuildJsonRpcError(nullptr, -32700, ErrorMessage));
		return SendHttpResponse(Client, Payload, TEXT("application/json"), 400);
	}

	ProcessMessage(Client, JsonMessage);
	return true;
}

bool FMCPProtocolServerRunnable::SendHttpResponse(TSharedPtr<FSocket> Client, const FString& Body, const FString& ContentType, int32 StatusCode, const TMap<FString, FString>& ExtraHeaders) const
{
	if (!Client.IsValid())
	{
		return false;
	}

	const FTCHARToUTF8 BodyUtf8(*Body);
	FString ResponseHeaders = FString::Printf(TEXT("HTTP/1.1 %d %s\r\n"), StatusCode, *GetStatusText(StatusCode));
	ResponseHeaders += FString::Printf(TEXT("Content-Type: %s\r\n"), *ContentType);
	ResponseHeaders += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyUtf8.Length());
	ResponseHeaders += TEXT("Connection: close\r\n");
	ResponseHeaders += TEXT("mcp-protocol-version: 2025-06-18\r\n");
	if (!SessionId.IsEmpty())
	{
		ResponseHeaders += FString::Printf(TEXT("mcp-session-id: %s\r\n"), *SessionId);
	}

	for (const auto& Header : ExtraHeaders)
	{
		ResponseHeaders += FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
	}

	ResponseHeaders += TEXT("\r\n");

	return SendAll(Client, ResponseHeaders + Body);
}

bool FMCPProtocolServerRunnable::SendSseResponse(TSharedPtr<FSocket> Client, const FString& JsonPayload, int32 StatusCode, const TMap<FString, FString>& ExtraHeaders) const
{
	TMap<FString, FString> Headers = ExtraHeaders;
	Headers.Add(TEXT("Cache-Control"), TEXT("no-cache, no-transform"));
	Headers.Add(TEXT("Connection"), TEXT("close"));

	const FString Body = FString::Printf(TEXT("data: %s\n\n"), *JsonPayload);
	return SendHttpResponse(Client, Body, TEXT("text/event-stream"), StatusCode, Headers);
}

void FMCPProtocolServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid())
	{
		SendHttpResponse(Client, TEXT(""), TEXT("application/json"), 400);
		return;
	}

	const bool bHasMethod = Message->HasField(TEXT("method"));
	const TSharedPtr<FJsonValue> IdValue = Message->TryGetField(TEXT("id"));
	const bool bIsNotification = !IdValue.IsValid();

	if (!bHasMethod)
	{
		SendHttpResponse(Client, TEXT(""), TEXT("application/json"), 202);
		return;
	}

	if (!Message->HasTypedField<EJson::String>(TEXT("method")))
	{
		const FString Payload = SerializeJsonObject(BuildJsonRpcError(IdValue, -32600, TEXT("Invalid request method")));
		SendHttpResponse(Client, Payload, TEXT("application/json"), 400);
		return;
	}

	const FString Method = Message->GetStringField(TEXT("method"));

	if (Method.Equals(TEXT("initialize")))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-06-18"));

		TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), MakeShared<FJsonObject>());
		Result->SetObjectField(TEXT("capabilities"), Capabilities);

		TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("UnrealMCP"));
		ServerInfo->SetStringField(TEXT("version"), TEXT("0.1"));
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		Result->SetStringField(TEXT("instructions"), TEXT("Unreal MCP Streamable HTTP endpoint"));

		const FString Payload = SerializeJsonObject(BuildJsonRpcResponse(IdValue, Result));
		SendSseResponse(Client, Payload, 200);
		return;
	}

	if (Method.Equals(TEXT("tools/list")))
	{
		TSharedPtr<FJsonObject> ToolsResult = BuildToolsPayload();
		const FString Payload = SerializeJsonObject(BuildJsonRpcResponse(IdValue, ToolsResult));
		SendSseResponse(Client, Payload, 200);
		return;
	}

	if (Method.Equals(TEXT("tools/call")))
	{
		if (!Message->HasTypedField<EJson::Object>(TEXT("params")))
		{
			const FString Payload = SerializeJsonObject(BuildJsonRpcError(IdValue, -32602, TEXT("Missing params for tools/call")));
			SendHttpResponse(Client, Payload, TEXT("application/json"), 400);
			return;
		}

		const TSharedPtr<FJsonObject> Params = Message->GetObjectField(TEXT("params"));
		if (!Params->HasField(TEXT("name")))
		{
			const FString Payload = SerializeJsonObject(BuildJsonRpcError(IdValue, -32602, TEXT("Missing tool name")));
			SendHttpResponse(Client, Payload, TEXT("application/json"), 400);
			return;
		}

		const FString ToolName = Params->GetStringField(TEXT("name"));
		TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
		if (Params->HasTypedField<EJson::Object>(TEXT("arguments")))
		{
			Arguments = Params->GetObjectField(TEXT("arguments"));
		}

		const FString RawResult = Bridge->ExecuteCommand(ToolName, Arguments);

		bool bSuccess = true;
		FString CommandError;
		TSharedPtr<FJsonObject> ParsedResult;
		if (FMCPJsonHelpers::ParseJson(RawResult, ParsedResult, CommandError))
		{
			if (ParsedResult->HasField(TEXT("status")))
			{
				const FString Status = ParsedResult->GetStringField(TEXT("status"));
				bSuccess = Status.Equals(TEXT("success"), ESearchCase::IgnoreCase);
			}
			else if (ParsedResult->HasField(TEXT("success")))
			{
				bSuccess = ParsedResult->GetBoolField(TEXT("success"));
			}
		}
		else
		{
			bSuccess = false;
			CommandError = CommandError.IsEmpty() ? TEXT("Failed to parse command response") : CommandError;
		}

		FString TextContent = RawResult;
		if (ParsedResult.IsValid() && ParsedResult->HasTypedField<EJson::Object>(TEXT("result")))
		{
			TextContent = FMCPJsonHelpers::SerializeJson(ParsedResult->GetObjectField(TEXT("result")));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ContentArray;

		TSharedPtr<FJsonObject> TextObj = MakeShared<FJsonObject>();
		TextObj->SetStringField(TEXT("type"), TEXT("text"));
		TextObj->SetStringField(TEXT("text"), bSuccess ? TextContent : (CommandError.IsEmpty() ? TextContent : CommandError));
		ContentArray.Add(MakeShared<FJsonValueObject>(TextObj));

		Result->SetArrayField(TEXT("content"), ContentArray);
		Result->SetBoolField(TEXT("isError"), !bSuccess);

		const FString Payload = SerializeJsonObject(BuildJsonRpcResponse(IdValue, Result));
		SendSseResponse(Client, Payload, 200);
		return;
	}

	// Notifications without IDs get a simple acknowledgement
	if (bIsNotification)
	{
		SendHttpResponse(Client, TEXT(""), TEXT("application/json"), 202);
		return;
	}

	const FString Payload = SerializeJsonObject(BuildJsonRpcError(IdValue, -32601, FString::Printf(TEXT("Unknown method: %s"), *Method)));
	SendHttpResponse(Client, Payload, TEXT("application/json"), 400);
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

TSharedPtr<FJsonObject> FMCPProtocolServerRunnable::BuildToolsPayload() const
{
	TSharedPtr<FJsonObject> ToolsObj = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	auto AddTool = [&ToolsArray](const FString& Name, const FString& Description)
	{
		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Name);
		ToolObj->SetStringField(TEXT("description"), Description);

		TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
		InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		InputSchema->SetBoolField(TEXT("additionalProperties"), true);
		ToolObj->SetObjectField(TEXT("inputSchema"), InputSchema);

		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	};

	// Note: this list mirrors the switch in UUnrealMCPBridge::ExecuteCommand
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
	return ToolsObj;
}
